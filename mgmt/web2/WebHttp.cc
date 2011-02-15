/** @file

  A brief file description

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

 /***************************************/
/****************************************************************************
 *
 *  WebHttp.cc - code to process requests, and create responses
 *
 *
 ****************************************************************************/

#include "libts.h"
#include "ink_platform.h"
#include "ink_unused.h" /* MAGIC_EDITING_TAG */

#include "SimpleTokenizer.h"

#include "WebCompatibility.h"
#include "WebHttp.h"
#include "WebHttpAuth.h"
#include "WebHttpContext.h"
#include "WebHttpLog.h"
#include "WebHttpMessage.h"
#include "WebHttpRender.h"
#include "WebHttpSession.h"
#include "WebHttpTree.h"
#include "WebOverview.h"
#include "WebConfig.h"

#include "mgmtapi.h"
//#include "I_AccCrypto.h"
#include "LocalManager.h"
#include "WebMgmtUtils.h"
#include "MgmtUtils.h"
#include "EnvBlock.h"
#include "CfgContextUtils.h"

#include "ConfigAPI.h"
#include "SysAPI.h"

#include "openssl/ssl.h"
#include "openssl/err.h"
#include "openssl/crypto.h"
#if !defined(_WIN32)
// Ugly hack - define HEAP_H and STACK_H to prevent stuff from the
// template library from being included which SUNPRO CC does not not
// like.
#define HEAP_H
#define STACK_H
#endif // !defined(_WIN32)


//-------------------------------------------------------------------------
// defines
//-------------------------------------------------------------------------

#ifndef _WIN32
#define DIR_MODE S_IRWXU
#define FILE_MODE S_IRWXU
#else
#define FILE_MODE S_IWRITE
#endif

#define MAX_ARGS         10
#define MAX_TMP_BUF_LEN  1024

//-------------------------------------------------------------------------
// types
//-------------------------------------------------------------------------

typedef int (*WebHttpHandler) (WebHttpContext * whc, const char *file);

//-------------------------------------------------------------------------
// globals
//-------------------------------------------------------------------------

// only allow access to specific files on the autoconf port
static InkHashTable *g_autoconf_allow_ht = 0;

static InkHashTable *g_submit_bindings_ht = 0;
static InkHashTable *g_file_bindings_ht = 0;
static InkHashTable *g_extn_bindings_ht = 0;

InkHashTable *g_display_config_ht = 0;

//-------------------------------------------------------------------------
// prototypes
//-------------------------------------------------------------------------

void spawn_script(WebHttpContext * whc, char *script, char **args);

//-------------------------------------------------------------------------
// record_version_valid
//-------------------------------------------------------------------------

static bool
record_version_valid(char *record_version)
{
  int old_version, old_pid, cur_version;
  // coverity[secure_coding]
  if (sscanf(record_version, "%d:%d", &old_pid, &old_version) == 2 && old_version >= 0) {
    cur_version = RecGetRecordUpdateCount(RECT_CONFIG);
    //fix me --> lmgmt->record_data->pid
    // TODO: need to check the PID ??
    //    if (cur_version != old_version || lmgmt->record_data->pid != old_pid) {
    if (cur_version != old_version) {
      // we are out of date since the version number has been incremented
      return false;
    } else {
      return true;
    }
  }
  // bad format, return false to be safe
  return false;
}

//-------------------------------------------------------------------------
// set_record_value
//-------------------------------------------------------------------------

static bool
set_record_value(WebHttpContext * whc, const char *rec, const char *value)
{
  MgmtData varValue;
  char *record;
  char *script = NULL;
  char *script_path;

  if (rec == NULL) {
    return false;
  }
  if (value == NULL) {
    value = "";
  }
  // INKqa11771: exec script that associates with a record
  record = xstrdup(rec);
  if ((script = strchr(record, ':'))) {
    *script = '\0';
    script++;
  }
  // FIXME: If someone else has already added a NOTE or WARN, then we
  // won't be able to add anymore.  This is desired for
  // handle_submit_update, but going forward, we'll need a more
  // general mechanism.

  varValue.setFromName(record);
  if (varValue.compareFromString(value) == false) {
    if (recordValidityCheck(record, value)) {
      if (recordRestartCheck(record)) {
        ink_hash_table_insert(whc->submit_note_ht, record, NULL);
        if (!(whc->request_state & WEB_HTTP_STATE_SUBMIT_NOTE)) {
          HtmlRndrText(whc->submit_note, whc->lang_dict_ht, HTML_ID_RESTART_REQUIRED);
          HtmlRndrBr(whc->submit_note);
        }
        whc->request_state |= WEB_HTTP_STATE_SUBMIT_NOTE;
      }
      varSetFromStr(record, value);

#ifndef _WIN32
      if (script) {
        const char *args[MAX_ARGS + 1];
        for (int i = 0; i < MAX_ARGS; i++)
          args[i] = NULL;
        script_path = WebHttpAddDocRoot_Xmalloc(whc, script);
        args[0] = script_path;
        args[1] = value;
        processSpawn(&args[0], NULL, NULL, NULL, false, false);
        xfree(script_path);
      }
#endif
    } else {
      ink_hash_table_insert(whc->submit_warn_ht, record, NULL);
      if (!(whc->request_state & WEB_HTTP_STATE_SUBMIT_WARN)) {
        HtmlRndrText(whc->submit_warn, whc->lang_dict_ht, HTML_ID_INVALID_ENTRY);
        HtmlRndrBr(whc->submit_warn);
      }
      whc->request_state |= WEB_HTTP_STATE_SUBMIT_WARN;
    }
  }
  xfree(record);
  return true;
}

//-------------------------------------------------------------------------
// spawn_cgi
//-------------------------------------------------------------------------

#if defined(_WIN32)

// adjustCmdLine
//
// This function is used for constructing a command line from a CGI
// scripting program because Windows doesn't know how to execute a
// script.  For example, instead of executing "blacklist.cgi", we need
// to tell Windows to execute "perl.exe blacklist.cgi".

static void
adjustCmdLine(char *cmdLine, int cmdline_len, const char *cgiFullPath)
{
  char line[1024 + 1];
  char *interpreter = NULL;

  FILE *f = fopen(cgiFullPath, "r");
  if (f != NULL) {
    if (fgets(line, 1024, f) != NULL) {
      int n = strlen(line);
      if (n > 2 && strncmp(line, "#!", 2) == 0 && line[n - 1] == '\n') {
        line[n - 1] = '\0';
        interpreter = line + 2;
      }
    }
    fclose(f);
  }

  if (interpreter) {
    snprintf(cmdLine, cmdline_len, "\"%s\" \"%s\"", interpreter, cgiFullPath);
  } else {
    ink_strncpy(cmdLine, cgiFullPath, cmdline_len);
  }
  return;
}

#endif

int
spawn_cgi(WebHttpContext * whc, const char *cgi_path, char **args, bool nowait, bool run_as_root)
{

  struct stat info;
  const char *query_string;
  textBuffer query_string_tb(MAX_TMP_BUF_LEN);
  int qlen = 0;
  char content_length_buffer[10];
  EnvBlock env;
  bool success = false;
  const char *a[MAX_ARGS + 2];
  int i;

  httpMessage *request = whc->request;
  textBuffer *replyMsg = whc->response_bdy;
  httpResponse *answerHdr = whc->response_hdr;

  // check if file exists
  if (stat(cgi_path, &info) < 0) {
    WebHttpSetErrorResponse(whc, STATUS_NOT_FOUND);
    answerHdr->setStatus(STATUS_NOT_FOUND);
    return WEB_HTTP_ERR_REQUEST_ERROR;
  }
  // initialize arguments
  for (i = 0; i < MAX_ARGS + 2; i++)
    a[i] = NULL;
  a[0] = cgi_path;
  for (i = 1; i < MAX_ARGS + 1 && args && args[i - 1]; i++)
    a[i] = args[i - 1];

  // initialize environment
  if (request->getContentType() != NULL) {
    env.setVar("CONTENT_TYPE", request->getContentType());
  }
  if (request->getMethod() == METHOD_POST) {
    env.setVar("REQUEST_METHOD", "POST");
    query_string = request->getBody();

  } else if (request->getMethod() == METHOD_GET) {
    env.setVar("REQUEST_METHOD", "GET");
    query_string = request->getQuery();

  } else {
    answerHdr->setStatus(STATUS_NOT_IMPLEMENTED);
    WebHttpSetErrorResponse(whc, STATUS_NOT_IMPLEMENTED);
    return WEB_HTTP_ERR_REQUEST_ERROR;;
  }
  if (query_string != NULL) {

    // use getConLen() to handle binary
    qlen = request->getConLen();
    if (qlen <= 0)
      qlen = strlen(query_string);
    snprintf(content_length_buffer, sizeof(content_length_buffer), "%d", qlen);
    env.setVar("CONTENT_LENGTH", content_length_buffer);
    env.setVar("QUERY_STRING", query_string);

    query_string_tb.copyFrom(query_string, qlen);
  }
#ifndef _WIN32
  if (processSpawn(&a[0], &env, &query_string_tb, replyMsg, nowait, run_as_root) != 0) {
    mgmt_elog(stderr, "[spawn_cgi] Unable to fork child process\n");
    WebHttpSetErrorResponse(whc, STATUS_INTERNAL_SERVER_ERROR);
    answerHdr->setStatus(STATUS_INTERNAL_SERVER_ERROR);

  } else {
    success = true;
  }
#else

  char buffer[1024];
  char cmdLine[PATH_MAX * 2 + 6];
  DWORD nbytes = 0;

  SECURITY_ATTRIBUTES saAttr;
  saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
  saAttr.bInheritHandle = TRUE;
  saAttr.lpSecurityDescriptor = NULL;

  // STDIN
  HANDLE hChildStdinR = NULL;
  HANDLE hChildStdinW = NULL;

  CreatePipe(&hChildStdinR, &hChildStdinW, &saAttr, 0);

  // Dup to NULL and set inheritable to FALSE so that
  // it won't be inherited by the child process
  DuplicateHandle(GetCurrentProcess(), hChildStdinW, GetCurrentProcess(), NULL, 0, FALSE, DUPLICATE_SAME_ACCESS);

  // STDOUT
  HANDLE hChildStdoutR = NULL;
  HANDLE hChildStdoutW = NULL;

  CreatePipe(&hChildStdoutR, &hChildStdoutW, &saAttr, 0);

  // Dup to NULL and set inheritable to FALSE so that
  // it won't be inherited by the child process
  DuplicateHandle(GetCurrentProcess(), hChildStdoutR, GetCurrentProcess(), NULL, 0, FALSE, DUPLICATE_SAME_ACCESS);

  STARTUPINFO suInfo;
  PROCESS_INFORMATION procInfo;
  ZeroMemory((PVOID) & suInfo, sizeof(suInfo));

  // hide the new console window from the user
  suInfo.cb = sizeof(STARTUPINFO);
  suInfo.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
  suInfo.wShowWindow = SW_HIDE;
  suInfo.hStdInput = hChildStdinR;
  suInfo.hStdOutput = hChildStdoutW;
  suInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);

  adjustCmdLine(cmdLine, sizeof(cmdLine), cgi_path);

  if (CreateProcess(NULL, cmdLine, NULL,        // FIX THIS: process security attributes
                    NULL,       // FIX THIS: thread security attributes
                    TRUE,       // make handles inheritable
                    0,          // FIX THIS: specify a priority
                    env.toString(), ts_base_dir,        // make script run from TSBase
                    &suInfo, &procInfo) == FALSE) {

    mgmt_elog(stderr, "[spawn_cgi] CreateProcess error: %s\n", ink_last_err());
    WebHttpSetErrorResponse(whc, STATUS_INTERNAL_SERVER_ERROR);
    answerHdr->setStatus(STATUS_INTERNAL_SERVER_ERROR);

  } else {

    CloseHandle(hChildStdinR);
    if (request->getMethod() == METHOD_POST && query_string != NULL) {
      WriteFile(hChildStdinW, query_string, qlen, &nbytes, NULL);
    }
    CloseHandle(hChildStdinW);

    CloseHandle(hChildStdoutW);
    while (ReadFile(hChildStdoutR, buffer, 1024, &nbytes, NULL) == TRUE) {
      if (nbytes == 0) {
        break;
      }
      replyMsg->copyFrom(buffer, nbytes);
    }
    CloseHandle(hChildStdoutR);
    success = true;
  }

#endif

  // was this a plugin callout?
  if (whc->request_state & WEB_HTTP_STATE_PLUGIN) {
    // notify server plugin to update its config
    if (success == true && query_string != NULL) {
      char *plugin_name = new char[qlen];
      const char *tmp = strstr(query_string, "INK_PLUGIN_NAME=");
      if (tmp != NULL) {
        tmp += strlen("INK_PLUGIN_NAME=");
        for (i = 0; *tmp != '&' && *tmp != '\0'; tmp++) {
          plugin_name[i++] = *tmp;
        }
        plugin_name[i] = '\0';
        substituteUnsafeChars(plugin_name);
        lmgmt->signalEvent(MGMT_EVENT_PLUGIN_CONFIG_UPDATE, plugin_name);
      }
      delete[]plugin_name;
    }
  }

  return WEB_HTTP_ERR_OKAY;

}
//-------------------------------------------------------------------------
// handle_cgi_extn
//-------------------------------------------------------------------------

static int
handle_cgi_extn(WebHttpContext * whc, const char *file)
{
  NOWARN_UNUSED(file);
  int err;
  char *cgi_path;
  whc->response_hdr->setCachable(0);
  whc->response_hdr->setStatus(STATUS_OK);
  whc->response_hdr->setContentType(TEXT_HTML);
  cgi_path = WebHttpAddDocRoot_Xmalloc(whc, whc->request->getFile());
  err = spawn_cgi(whc, cgi_path, NULL, false, false);
  xfree(cgi_path);
  return err;
}

//-------------------------------------------------------------------------
// handle_ink_extn
//-------------------------------------------------------------------------

static int
handle_ink_extn(WebHttpContext * whc, const char *file)
{
  int err;
  if ((err = WebHttpRender(whc, file)) == WEB_HTTP_ERR_OKAY) {
    whc->response_hdr->setStatus(STATUS_OK);
    whc->response_hdr->setLength(whc->response_bdy->spaceUsed());
    whc->response_hdr->setContentType(TEXT_HTML);
  }
  return err;
}

//-------------------------------------------------------------------------
// handle_chart
//-------------------------------------------------------------------------

static int
handle_chart(WebHttpContext * whc, const char *file)
{
  NOWARN_UNUSED(file);
  //-----------------------------------------------------------------------
  // FIXME: HARD-CODED HTML HELL!!!
  //-----------------------------------------------------------------------

  // Note that chart.cgi is a special case so it can not be handled
  // like our other submit_bindings; the browswer can access the cgi
  // either by a GET/query or by a POST/body combo.

  int err = WEB_HTTP_ERR_OKAY;

  httpMessage *request = whc->request;
  textBuffer *replyMsg = whc->response_bdy;
  httpResponse *answerHdr = whc->response_hdr;
  InkHashTable *post_data_ht, *params;

  char *varName = NULL;
  int varNameLen;
  char tmpVal[MAX_VAL_LENGTH];
  bool postForm = true;
  bool clusterGraph = false;

  //800, 410
  static const char dimensions[] = "width=\"1600\" height=\"1200\"";
  static const char multiGraph[] = "Inktomi Real-time Graphing";
  int numGraphs = 0;
  const int totalNumGraphs = 10;
  char numGraphStr[8];
  char *theGraphs[totalNumGraphs];
  const char *theGraphNames[totalNumGraphs];
  const char *graphNames[] = {
    "Document Hit Rate", "Bandwidth Savings", "Cache Percent Free",
    "Open Server Connections", "Open Client Connections",
    "Cache Transfers In Progress", "Client Throughput",
    "Transactions Per Second", "Host Database Hit Rate",
    "DNS Lookups Per Second"
  };

  static const char str1[] = "<html>\n" "<title>";
  static const char str1_5[] =
    "</title>\n" "<body><b> No variable(s) were selected for graphing. </b></body>\n" "</html>\n";
  static const char str2[] =
    "</title>\n"
    "<body bgcolor=\"#C0C0C0\" onResize=\"resize()\" onLoad=\"resize()\" "
    " topmargin=\"0\" leftmargin=\"0\" marginwidth=\"0\" marginheight=\"0\">\n"
    "<SCRIPT LANGUAGE=\"JavaScript\">\n"
    "   function myFunc(page, winName) {\n"
    "          window.open(page, winName, \"width=850,height=435,status,resizable=yes\");\n"
    "   }\n"
    "   function resize() {\n"
    "	var w_newWidth,w_newHeight;\n"
    "	var w_maxWidth=1600,w_maxHeight=1200;\n"
    "	if (navigator.appName.indexOf(\"Microsoft\") != -1)\n"
    "	{\n"
    "		w_newWidth=document.body.clientWidth;\n"
    "		w_newHeight=document.body.clientHeight;\n"
    "	} else {\n"
    "		var netscapeScrollWidth=15;\n"
    "		w_newWidth=window.innerWidth-netscapeScrollWidth;\n"
    "		w_newHeight=window.innerHeight-netscapeScrollWidth;\n"
    "	}\n"
    "	if (w_newWidth>w_maxWidth)\n"
    "		w_newWidth=w_maxWidth;\n"
    "	if (w_newHeight>w_maxHeight)\n"
    "		w_newHeight=w_maxHeight;\n"
    "	document.ink_chart.resizeFrame(w_newWidth,w_newHeight);\n"
    "        window.scroll(0,0);\n" "   }\n" "   window.onResize = resize;\n" "   window.onLoad = resize;\n"
    //kwt
    "   function closeTheBrowser() {\n"
    "   window.close();\n"
    "   }\n"
    "   function SnapshotAlert() {\n"
    "   window.alert(\"Snapshot is currently not supported on SSL connection.\");\n" "   }\n"
    //kwt
    "</SCRIPT>\n"
    "<applet NAME=\"ink_chart\" CODE=\"InktomiCharter.class\" " " ARCHIVE=\"/charting/InkChart.jar\" MAYSCRIPT ";
  static const char str3[] = ">\n<param name=ServerName value=\"";
  static const char str3_3[] = "\">\n<param name=ServerWebPort value=\"";
  static const char str3_4[] = "\">\n<param name=Graphs value=\"";
  static const char str3_5[] = "\">\n<param name=StatNames   value=\"";
  static const char str3_6[] = "\">\n<param name=SSL value=\"";
  static const char str4[] = "\">\n</applet>\n</body>\n</html>\n";

  static const int str1Len = strlen(str1);
  static const int str1_5Len = strlen(str1_5);
  static const int str2Len = strlen(str2);
  static const int str3Len = strlen(str3);
  static const int str3_3Len = strlen(str3_3);
  static const int str3_4Len = strlen(str3_4);
  static const int str3_5Len = strlen(str3_5);
  static const int str3_6Len = strlen(str3_6);
  static const int str4Len = strlen(str4);

  // The graph Generator is a POST form, while the cluster graphs are
  // GET forms.  If we get nothing, assume that we have a postForm.
  post_data_ht = processFormSubmission(request->getBody());
  if (post_data_ht == NULL) {
    postForm = false;
    params = whc->query_data_ht;
    // If we still didn't get anything, there is nothing to be had
    if (params == NULL) {
      err = WEB_HTTP_ERR_REQUEST_ERROR;
      goto Ldone;
    }
  } else {
    params = post_data_ht;
  }

  if (postForm == false) {
    // We are trying to generate a cluster graph for a node variable
    int ink_hash_lookup_result = ink_hash_table_lookup(params, "cluster", (void **) &varName);
    if (!ink_hash_lookup_result || varName == NULL) {
      mgmt_log(stderr, "Invalid Graph Submission No graph will be generated\n");
      err = WEB_HTTP_ERR_REQUEST_ERROR;
      goto Ldone;
    }
    clusterGraph = true;
  } else {
    for (int i = 0; i < totalNumGraphs; i++) {
      if (ink_hash_table_lookup(params, graphNames[i], (void **) &varName)) {
        theGraphs[numGraphs] = (char *) varName;
        theGraphNames[numGraphs] = graphNames[i];
        numGraphs += 1;
      }
    }
    clusterGraph = false;
  }

  varNameLen = varName ? strlen(varName) : 0;

  // Build the reply
  replyMsg->copyFrom(str1, str1Len);
  if (clusterGraph == true && varName) {
    replyMsg->copyFrom(varName, varNameLen);
  } else {
    replyMsg->copyFrom(multiGraph, strlen(multiGraph));
    if (numGraphs == 0) {
      replyMsg->copyFrom(str1_5, str1_5Len);
      answerHdr->setStatus(STATUS_OK);
      goto Ldone;
    }
  }
  replyMsg->copyFrom(str2, str2Len);
  replyMsg->copyFrom(dimensions, strlen(dimensions));

  replyMsg->copyFrom(str3, str3Len);
  varStrFromName("proxy.node.hostname_FQ", tmpVal, MAX_VAL_LENGTH);
  replyMsg->copyFrom(tmpVal, strlen(tmpVal));

  replyMsg->copyFrom(str3_3, str3_3Len);
  varStrFromName("proxy.config.admin.web_interface_port", tmpVal, MAX_VAL_LENGTH);
  replyMsg->copyFrom(tmpVal, strlen(tmpVal));

  replyMsg->copyFrom(str3_4, str3_4Len);
  if (clusterGraph == true) {
    replyMsg->copyFrom("CLUSTER", 7);
  } else {
    snprintf(numGraphStr, sizeof(numGraphStr), "%d", numGraphs);
    replyMsg->copyFrom(numGraphStr, strlen(numGraphStr));
  }

  replyMsg->copyFrom(str3_5, str3_5Len);
  if (clusterGraph == true) {
    replyMsg->copyFrom(varName, varNameLen);
  } else {
    for (int j = 1; j < numGraphs; j++) {
      replyMsg->copyFrom(theGraphs[j], strlen(theGraphs[j]));
      replyMsg->copyFrom(",", 1);
      replyMsg->copyFrom(theGraphNames[j], strlen(theGraphNames[j]));

      replyMsg->copyFrom(",", 1);
    }
    replyMsg->copyFrom(theGraphs[0], strlen(theGraphs[0]));
    replyMsg->copyFrom(",", 1);
    replyMsg->copyFrom(theGraphNames[0], strlen(theGraphNames[0]));
  }

  replyMsg->copyFrom(str3_6, str3_6Len);
  if (whc->server_state & WEB_HTTP_SERVER_STATE_SSL_ENABLED) {
    replyMsg->copyFrom("enabled", strlen("enabled"));
  } else {
    replyMsg->copyFrom("disabled", strlen("disabled"));
  }

  replyMsg->copyFrom(str4, str4Len);
  answerHdr->setLength(strlen(replyMsg->bufPtr()));

Ldone:
  if (post_data_ht) {
    ink_hash_table_destroy_and_xfree_values(post_data_ht);
  }
  return err;

}

//-------------------------------------------------------------------------
// handle_record_info
//
// Warning!!! This is really hacky since we should not be directly
// accessing the librecords data structures.  Just do this here
// tempoarily until we can have something better.
//-------------------------------------------------------------------------

#include "P_RecCore.h"

#define LINE_SIZE 512
#define BUF_SIZE 128
#define NULL_STR "NULL"

#undef LINE_SIZE
#undef BUF_SIZE
#undef NULL_STR

static int
handle_config_files(WebHttpContext * whc, const char *file)
{
  NOWARN_UNUSED(file);
  return WebHttpRender(whc, HTML_FILE_ALL_CONFIG);
}

static int
handle_debug_logs(WebHttpContext * whc, const char *file)
{
  NOWARN_UNUSED(file);
  return WebHttpRender(whc, HTML_VIEW_DEBUG_LOGS_FILE);
}

//-------------------------------------------------------------------------
// handle_synthetic
//-------------------------------------------------------------------------

static int
handle_synthetic(WebHttpContext * whc, const char *file)
{
  NOWARN_UNUSED(file);
  char buffer[28];
  char cur = 'a';
  whc->response_hdr->setContentType(TEXT_PLAIN);
  whc->response_hdr->setStatus(STATUS_OK);
  buffer[26] = '\n';
  buffer[27] = '\0';
  for (int i = 0; i < 26; i++) {
    *(buffer + i) = cur;
    cur++;
  }
  for (int j = 0; j < 60; j++) {
    whc->response_bdy->copyFrom(buffer, 27);
  }
  return WEB_HTTP_ERR_OKAY;
}

//-------------------------------------------------------------------------
// handle_submit_alarm
//-------------------------------------------------------------------------

static int
handle_submit_alarm(WebHttpContext * whc, const char *file)
{
  NOWARN_UNUSED(file);
  resolveAlarm(whc->post_data_ht);
  whc->top_level_render_file = xstrdup(HTML_ALARM_FILE);
  return handle_ink_extn(whc, HTML_ALARM_FILE);
}

//-------------------------------------------------------------------------
// handle_submit_mgmt_auth
//-------------------------------------------------------------------------

// set_admin_passwd (sub-function)
static inline void
set_admin_passwd(WebHttpContext * whc)
{

  char *admin_orig_epasswd;
  char *admin_old_passwd;
  char *admin_old_epasswd;
  char *admin_new_passwd;
  char *admin_new_passwd_retype;
  char *admin_new_epasswd;

  char empty_str[1];
  *empty_str = '\0';

  if (!ink_hash_table_lookup(whc->post_data_ht, "admin_old_passwd", (void **) &admin_old_passwd))
    admin_old_passwd = NULL;
  if (!ink_hash_table_lookup(whc->post_data_ht, "admin_new_passwd", (void **) &admin_new_passwd))
    admin_new_passwd = NULL;
  if (!ink_hash_table_lookup(whc->post_data_ht, "admin_new_passwd_retype", (void **) &admin_new_passwd_retype))
    admin_new_passwd_retype = NULL;

  if ((admin_old_passwd != NULL) || (admin_new_passwd != NULL) || (admin_new_passwd_retype != NULL)) {

    if (admin_old_passwd == NULL)
      admin_old_passwd = empty_str;
    if (admin_new_passwd == NULL)
      admin_new_passwd = empty_str;
    if (admin_new_passwd_retype == NULL)
      admin_new_passwd_retype = empty_str;

    admin_orig_epasswd = (char *) alloca(TS_ENCRYPT_PASSWD_LEN + 1);
    varStrFromName("proxy.config.admin.admin_password", admin_orig_epasswd, TS_ENCRYPT_PASSWD_LEN + 1);

    // INKqa12084: do not encrypt password if empty_str
    if (strcmp(admin_old_passwd, empty_str) == 0) {
      admin_old_epasswd = xstrdup(empty_str);
    } else {
      TSEncryptPassword(admin_old_passwd, &admin_old_epasswd);
    }

    if (strncmp(admin_old_epasswd, admin_orig_epasswd, TS_ENCRYPT_PASSWD_LEN) == 0) {
      if (strcmp(admin_new_passwd, admin_new_passwd_retype) == 0) {
        // INKqa12084: do not encrypt password if empty_str
        if (strcmp(admin_new_passwd, empty_str) == 0) {
          admin_new_epasswd = xstrdup(empty_str);
        } else {
          TSEncryptPassword(admin_new_passwd, &admin_new_epasswd);
        }

        set_record_value(whc, "proxy.config.admin.admin_password", admin_new_epasswd);
        xfree(admin_new_epasswd);
        whc->request_state |= WEB_HTTP_STATE_SUBMIT_NOTE;
        HtmlRndrText(whc->submit_note, whc->lang_dict_ht, HTML_ID_NEW_ADMIN_PASSWD_SET);
        HtmlRndrBr(whc->submit_note);
      } else {
        ink_hash_table_insert(whc->submit_warn_ht, "proxy.config.admin.admin_password", NULL);
        whc->request_state |= WEB_HTTP_STATE_SUBMIT_WARN;
        HtmlRndrText(whc->submit_warn, whc->lang_dict_ht, HTML_ID_NEW_PASSWD_MISTYPE);
        HtmlRndrBr(whc->submit_warn);
      }
    } else {
      ink_hash_table_insert(whc->submit_warn_ht, "proxy.config.admin.admin_password", NULL);
      whc->request_state |= WEB_HTTP_STATE_SUBMIT_WARN;
      HtmlRndrText(whc->submit_warn, whc->lang_dict_ht, HTML_ID_OLD_PASSWD_INCORRECT);
      HtmlRndrBr(whc->submit_warn);
    }
    xfree(admin_old_epasswd);
  }

}

static int
handle_submit_mgmt_auth(WebHttpContext * whc, const char *file)
{
  NOWARN_UNUSED(file);
  bool recs_out_of_date;
  char *value;
  char *cancel;
  char *record_version;
  char *submit_from_page;
  char *aa_session_id, *aa_user_count;
  char *aa_user, *aa_access, *aa_delete;
  char *aa_new_user, *aa_new_passwd, *aa_new_passwd_retype, *aa_new_access;
  char *aa_new_epasswd;
  char admin_user[MAX_VAL_LENGTH + 1];
  int user, user_count;
  TSCfgContext ctx;
  TSAdminAccessEle *ele;
  TSActionNeedT action_need;
  TSAccessT access_t;
  bool ctx_updated;

  char tmp_a[32];
  char tmp_b[32];

  char empty_str[1];
  *empty_str = '\0';

  // initialize pointers we may assign memeory to
  aa_new_epasswd = NULL;

  // check for submit_from_page
  if (ink_hash_table_lookup(whc->post_data_ht, "submit_from_page", (void **) &submit_from_page)) {
    ink_hash_table_delete(whc->post_data_ht, "submit_from_page");
    whc->top_level_render_file = xstrdup(submit_from_page);
  } else {
    submit_from_page = NULL;
  }

  // check for cancel
  if (ink_hash_table_lookup(whc->post_data_ht, "cancel", (void **) &cancel))
    goto Ldone;

  // check for record_version
  recs_out_of_date = true;
  if (ink_hash_table_lookup(whc->post_data_ht, "record_version", (void **) &record_version)) {
    recs_out_of_date = !record_version_valid(record_version);
    ink_hash_table_delete(whc->post_data_ht, "record_version");
    xfree(record_version);
  }
  if (recs_out_of_date)
    goto Lout_of_date;

  // proxy.config.admin.basic_auth
  if (ink_hash_table_lookup(whc->post_data_ht, "proxy.config.admin.basic_auth", (void **) &value))
    set_record_value(whc, "proxy.config.admin.basic_auth", value);

  // proxy.config.admin.admin_user
  if (ink_hash_table_lookup(whc->post_data_ht, "proxy.config.admin.admin_user", (void **) &value))
    set_record_value(whc, "proxy.config.admin.admin_user", value);

  // proxy.config.admin.admin_password (call sub-function)
  set_admin_passwd(whc);

  // grab our session_id and user_count
  if (ink_hash_table_lookup(whc->post_data_ht, "session_id", (void **) &aa_session_id)) {
    if (!ink_hash_table_lookup(whc->post_data_ht, "user_count", (void **) &aa_user_count))
      goto Lunable_to_submit;
    // find our current session
    if (WebHttpSessionRetrieve(aa_session_id, (void **) &ctx) != WEB_HTTP_ERR_OKAY)
      goto Lout_of_date;
    // get new additional-user information
    if (!ink_hash_table_lookup(whc->post_data_ht, "new_user", (void **) &aa_new_user))
      aa_new_user = NULL;
    if (!ink_hash_table_lookup(whc->post_data_ht, "new_passwd", (void **) &aa_new_passwd))
      aa_new_passwd = NULL;
    if (!ink_hash_table_lookup(whc->post_data_ht, "new_passwd_retype", (void **) &aa_new_passwd_retype))
      aa_new_passwd_retype = NULL;
    if (!ink_hash_table_lookup(whc->post_data_ht, "new_access", (void **) &aa_new_access))
      aa_new_access = NULL;
    // check if the user is trying to add a new additional-user
    if (aa_new_user != NULL) {
      // kwt 12.March.2001 check for username length
      if (strlen(aa_new_user) > WEB_HTTP_AUTH_USER_MAX) {
        ink_hash_table_insert(whc->submit_warn_ht, "additional_administrative_accounts", NULL);
        ink_hash_table_insert(whc->submit_warn_ht, "add_new_administrative_user", NULL);
        whc->request_state |= WEB_HTTP_STATE_SUBMIT_WARN;
        HtmlRndrText(whc->submit_warn, whc->lang_dict_ht, HTML_ID_NEW_USERNAME_LENGTH);
        HtmlRndrBr(whc->submit_warn);
        aa_new_user = NULL;
      }
      // kwt
      if (aa_new_user != NULL) {
        if (aa_new_passwd == NULL)
          aa_new_passwd = empty_str;
        if (aa_new_passwd_retype == NULL)
          aa_new_passwd_retype = empty_str;
        if (strcmp(aa_new_passwd, aa_new_passwd_retype) == 0) {
          // allocating memory on aa_new_epasswd
          TSEncryptPassword(aa_new_passwd, &aa_new_epasswd);
        } else {
          ink_hash_table_insert(whc->submit_warn_ht, "additional_administrative_accounts", NULL);
          ink_hash_table_insert(whc->submit_warn_ht, "add_new_administrative_user", NULL);
          whc->request_state |= WEB_HTTP_STATE_SUBMIT_WARN;
          HtmlRndrText(whc->submit_warn, whc->lang_dict_ht, HTML_ID_NEW_PASSWD_MISTYPE);
          HtmlRndrBr(whc->submit_warn);
        }
      }
      // check if the new_user is the same as the proxy.config.admin.admin_user
      if (aa_new_user != NULL) {
        varStrFromName("proxy.config.admin.admin_user", admin_user, MAX_VAL_LENGTH + 1);
        if (strcmp(aa_new_user, admin_user) == 0) {
          ink_hash_table_insert(whc->submit_warn_ht, "additional_administrative_accounts", NULL);
          ink_hash_table_insert(whc->submit_warn_ht, "add_new_administrative_user", NULL);
          whc->request_state |= WEB_HTTP_STATE_SUBMIT_WARN;
          HtmlRndrText(whc->submit_warn, whc->lang_dict_ht, HTML_ID_NEW_USER_DUPLICATE);
          HtmlRndrBr(whc->submit_warn);
          aa_new_user = NULL;
        }
      }
    }
    // Walk through members and update settings in ctx backwards.
    // Client submitted values should be in the same order as the ctx
    // since we originally created this page from the same ctx.
    // Looping backwards helps so that we can delete elements by
    // index.
    ctx_updated = false;
    user_count = ink_atoi(aa_user_count);
    for (user = user_count - 1; user >= 0; user--) {
      snprintf(tmp_a, sizeof(tmp_a), "user:%d", user);
      snprintf(tmp_b, sizeof(tmp_b), "access:%d", user);
      if (ink_hash_table_lookup(whc->post_data_ht, tmp_a, (void **) &aa_user) &&
          ink_hash_table_lookup(whc->post_data_ht, tmp_b, (void **) &aa_access)) {
        snprintf(tmp_a, sizeof(tmp_a), "delete:%d", user);
        if (ink_hash_table_lookup(whc->post_data_ht, tmp_a, (void **) &aa_delete)) {
          TSCfgContextRemoveEleAt(ctx, user);
          ctx_updated = true;
          continue;
        }
        ele = (TSAdminAccessEle *) TSCfgContextGetEleAt(ctx, user);
        if (strcmp(ele->user, aa_user) != 0) {
          goto Lunable_to_submit;
        }
        if (aa_new_user && (strcmp(aa_new_user, aa_user) == 0)) {
          ink_hash_table_insert(whc->submit_warn_ht, "additional_administrative_accounts", NULL);
          ink_hash_table_insert(whc->submit_warn_ht, "add_new_administrative_user", NULL);
          whc->request_state |= WEB_HTTP_STATE_SUBMIT_WARN;
          HtmlRndrText(whc->submit_warn, whc->lang_dict_ht, HTML_ID_NEW_USER_DUPLICATE);
          HtmlRndrBr(whc->submit_warn);
          aa_new_user = NULL;
        }
        access_t = (TSAccessT) ink_atoi(aa_access);
        if (ele->access != access_t) {
          ele->access = access_t;
          ctx_updated = true;
        }
      } else {
        goto Lunable_to_submit;
      }
    }
    // add new user
    if ((aa_new_user != NULL) && (aa_new_epasswd != NULL)) {
      ele = TSAdminAccessEleCreate();
      ele->user = xstrdup(aa_new_user);
      ele->password = xstrdup(aa_new_epasswd);
      // FIXME: no access for now, add back later?
      //ele->access = aa_new_access ? (TSAccessT)ink_atoi(aa_new_access) : TS_ACCESS_NONE;
      ele->access = TS_ACCESS_NONE;
      TSCfgContextAppendEle(ctx, (TSCfgEle *) ele);
      ctx_updated = true;
    }
    if (ctx_updated) {
      if (TSCfgContextCommit(ctx, &action_need, NULL) != TS_ERR_OKAY) {
        WebHttpSessionDelete(aa_session_id);
        goto Lout_of_date;
      }
      TSActionDo(action_need);
    }
    WebHttpSessionDelete(aa_session_id);
  } else {
    goto Lunable_to_submit;
  }
  goto Ldone;

Lout_of_date:
  whc->request_state |= WEB_HTTP_STATE_SUBMIT_WARN;
  HtmlRndrText(whc->submit_warn, whc->lang_dict_ht, HTML_ID_OUT_OF_DATE);
  HtmlRndrBr(whc->submit_warn);
  goto Ldone;

Lunable_to_submit:
  whc->request_state |= WEB_HTTP_STATE_SUBMIT_WARN;
  HtmlRndrText(whc->submit_warn, whc->lang_dict_ht, HTML_ID_UNABLE_TO_SUBMIT);
  HtmlRndrBr(whc->submit_warn);
  goto Ldone;

Ldone:
  if (aa_new_epasswd) {
    xfree(aa_new_epasswd);
  }
  return WebHttpRender(whc, HTML_MGMT_LOGIN_FILE);
}


//-------------------------------------------------------------------------
// handle_submit_view_logs
//-------------------------------------------------------------------------
static int
handle_submit_view_logs(WebHttpContext * whc, const char *file)
{
  NOWARN_UNUSED(file);
  int err;
  char *submit_from_page;
  char *nlines;
  char *substring;
  char *action = NULL;
  char *logfile = NULL;
  char tmp[MAX_TMP_BUF_LEN + 1];
  int file_size;
  time_t file_date_gmt;

  // check for submit_from_page
  if (ink_hash_table_lookup(whc->post_data_ht, "submit_from_page", (void **) &submit_from_page)) {
    ink_hash_table_delete(whc->post_data_ht, "submit_from_page");
    whc->top_level_render_file = xstrdup(submit_from_page);
  } else {
    submit_from_page = NULL;
  }

#if !defined(_WIN32)

  // handle remove/save file action before rendering
  if (!ink_hash_table_lookup(whc->post_data_ht, "logfile", (void **) &logfile))
    goto Ldone;
  if (!ink_hash_table_lookup(whc->post_data_ht, "action", (void **) &action))
    goto Ldone;
  if (!logfile || !action)
    goto Ldone;
  if (strcmp(logfile, "default") == 0)
    goto Ldone;

  if (strcmp(action, "view_last") == 0) {
    if (!ink_hash_table_lookup(whc->post_data_ht, "nlines", (void **) &nlines))
      goto Ldone;
    // 'nlines' entry is missing
    if (nlines == NULL) {
      ink_hash_table_insert(whc->submit_warn_ht, "view_last", NULL);
      goto Lmiss;
    }

  } else if (strcmp(action, "view_subset") == 0) {
    if (!ink_hash_table_lookup(whc->post_data_ht, "substring", (void **) &substring))
      goto Ldone;
    // 'substring' entry is missing
    if (substring == NULL) {
      ink_hash_table_insert(whc->submit_warn_ht, "view_subset", NULL);
      goto Lmiss;
    }

  } else if (strcmp(action, "remove") == 0) {

    snprintf(tmp, MAX_TMP_BUF_LEN, "/bin/rm -f %s", logfile);
    if (system(tmp)) {
      Debug("web2", "[handle_submit_view_logs] unable to execute \"%s\"", tmp);
      if (!(whc->request_state & WEB_HTTP_STATE_SUBMIT_WARN)) {
        whc->request_state |= WEB_HTTP_STATE_SUBMIT_WARN;
        HtmlRndrText(whc->submit_warn, whc->lang_dict_ht, HTML_ID_LOG_REMOVE_FAILED);
        HtmlRndrBr(whc->submit_warn);
      }
    } else {
      // done removal - remove from post_data_ht not to display previous action
      ink_hash_table_delete(whc->post_data_ht, "action");
      ink_hash_table_delete(whc->post_data_ht, "logfile");
      xfree(logfile);
      xfree(action);
    }
  } else if (strcmp(action, "save") == 0) {
    WebHandle h_file;
    if ((h_file = WebFileOpenR(logfile)) == WEB_HANDLE_INVALID) {
      Debug("web2", "[handle_submit_view_logs] unable to open logfile \"%s\"", logfile);

      if (!(whc->request_state & WEB_HTTP_STATE_SUBMIT_WARN)) {
        whc->request_state |= WEB_HTTP_STATE_SUBMIT_WARN;
        HtmlRndrText(whc->submit_warn, whc->lang_dict_ht, HTML_ID_LOG_SAVE_FAILED);
        HtmlRndrBr(whc->submit_warn);
      }
    } else {
      file_size = WebFileGetSize(h_file);
      file_date_gmt = WebFileGetDateGmt(h_file);
      whc->response_hdr->setStatus(STATUS_OK);
      whc->response_hdr->setLength(file_size);
      whc->response_hdr->setLastMod(file_date_gmt);
      whc->response_hdr->setContentType(TEXT_UNKNOWN);
      while (whc->response_bdy->rawReadFromFile(h_file) > 0);
      WebFileClose(h_file);
      return WEB_HTTP_ERR_OKAY;
    }
  } else {
    Debug("web2", "[handle_submit_view_logs] unknown action '%s' on '%s'", action, logfile);
  }
  goto Ldone;

#endif

Lmiss:
  if (!(whc->request_state & WEB_HTTP_STATE_SUBMIT_WARN)) {
    whc->request_state |= WEB_HTTP_STATE_SUBMIT_WARN;
    HtmlRndrText(whc->submit_warn, whc->lang_dict_ht, HTML_ID_MISSING_ENTRY);
    HtmlRndrBr(whc->submit_warn);
  }

Ldone:
  // nothing needs to be done, just start rendering
  if (submit_from_page) {
    err = WebHttpRender(whc, submit_from_page);
    xfree(submit_from_page);
  } else {
    err = WebHttpRender(whc, HTML_DEFAULT_MONITOR_FILE);
  }
  return err;
}


//-------------------------------------------------------------------------
// network configuration
//-------------------------------------------------------------------------


bool
NICCheck(WebHttpContext * whc, char *updown, char *arg)
{
  bool result;

  result = true;

  if (strcmp(updown, "0") == 0) {
    result = false;
    ink_hash_table_insert(whc->submit_warn_ht, arg, NULL);
    if (!(whc->request_state & WEB_HTTP_STATE_SUBMIT_WARN)) {
      HtmlRndrText(whc->submit_warn, whc->lang_dict_ht, HTML_ID_INVALID_ENTRY);
      HtmlRndrBr(whc->submit_warn);
    }
    whc->request_state |= WEB_HTTP_STATE_SUBMIT_WARN;
  }
  return result;
}

void
SetWarning(WebHttpContext * whc, char *arg)
{
  ink_hash_table_insert(whc->submit_warn_ht, arg, NULL);
  if (!(whc->request_state & WEB_HTTP_STATE_SUBMIT_WARN)) {
    HtmlRndrText(whc->submit_warn, whc->lang_dict_ht, HTML_ID_INVALID_ENTRY);
    HtmlRndrBr(whc->submit_warn);
  }
  whc->request_state |= WEB_HTTP_STATE_SUBMIT_WARN;
}


//-------------------------------------------------------------------------
// handle_default
//-------------------------------------------------------------------------

static int
handle_default(WebHttpContext * whc, const char *file)
{

  char *doc_root_file;
  int file_size;
  time_t file_date_gmt;
  WebHandle h_file;

  httpMessage *request = whc->request;
  httpResponse *response_hdr = whc->response_hdr;
  textBuffer *response_bdy = whc->response_bdy;

  const char *request_file = file;
  time_t request_file_ims;
  int request_file_len;

  // requests are supposed to begin with a "/"
  if (*request_file != '/') {
    response_hdr->setStatus(STATUS_NOT_FOUND);
    WebHttpSetErrorResponse(whc, STATUS_NOT_FOUND);
    return WEB_HTTP_ERR_REQUEST_ERROR;
  }
  // first, make sure there are no ..'s in path or root directory
  // access in name for security reasons
  if (strstr(request_file, "..") != NULL || strncmp(request_file, "//", 2) == 0) {
    response_hdr->setStatus(STATUS_FORBIDDEN);
    WebHttpSetErrorResponse(whc, STATUS_FORBIDDEN);
    return WEB_HTTP_ERR_REQUEST_ERROR;
  }

  if (strcmp("/", request_file) == 0) {
    request_file = whc->default_file;
  }
  // check file type and set document type if appropiate
  request_file_len = strlen(request_file);
  if (strcmp(request_file + (request_file_len - 4), ".htm") == 0) {
    response_hdr->setContentType(TEXT_HTML);
  } else if (strcmp(request_file + (request_file_len - 5), ".html") == 0) {
    response_hdr->setContentType(TEXT_HTML);
  } else if (strcmp(request_file + (request_file_len - 4), ".css") == 0) {
    response_hdr->setContentType(TEXT_CSS);
  } else if (strcmp(request_file + (request_file_len - 4), ".gif") == 0) {
    response_hdr->setContentType(IMAGE_GIF);
  } else if (strcmp(request_file + (request_file_len - 4), ".jpg") == 0) {
    response_hdr->setContentType(IMAGE_JPEG);
  } else if (strcmp(request_file + (request_file_len - 5), ".jpeg") == 0) {
    response_hdr->setContentType(IMAGE_JPEG);
  } else if (strcmp(request_file + (request_file_len - 4), ".png") == 0) {
    response_hdr->setContentType(IMAGE_PNG);
  } else if (strcmp(request_file + (request_file_len - 4), ".jar") == 0) {
    response_hdr->setContentType(APP_JAVA);
  } else if (strcmp(request_file + (request_file_len - 3), ".js") == 0) {
    response_hdr->setContentType(APP_JAVASCRIPT);
  } else if (strcmp(request_file + (request_file_len - 4), ".der") == 0) {
    response_hdr->setContentType(APP_X509);
  } else if (strcmp(request_file + (request_file_len - 4), ".dat") == 0) {
    response_hdr->setContentType(APP_AUTOCONFIG);
    response_hdr->setCachable(0);
  } else if (strcmp(request_file + (request_file_len - 4), ".pac") == 0) {
    response_hdr->setContentType(APP_AUTOCONFIG);
    // Fixed INKqa04312 - 02/21/1999 elam
    // We don't want anyone to cache .pac files.
    response_hdr->setCachable(0);
  } else if (strcmp(request_file + (request_file_len - 4), ".zip") == 0) {
    response_hdr->setContentType(APP_ZIP);
  } else {
    // don't serve file types that we don't know about; helps to lock
    // down the webserver.  for example, when serving files out the
    // etc/trafficserver/plugins directory, we don't want to allow the users to
    // access the .so/.dll plugin files.
    response_hdr->setStatus(STATUS_NOT_FOUND);
    WebHttpSetErrorResponse(whc, STATUS_NOT_FOUND);
    return WEB_HTTP_ERR_REQUEST_ERROR;
  }

  // append the appropriate doc_root on to the file
  doc_root_file = WebHttpAddDocRoot_Xmalloc(whc, request_file);

  // open the requested file
  if ((h_file = WebFileOpenR(doc_root_file)) == WEB_HANDLE_INVALID) {
    //could not find file
    xfree(doc_root_file);
    response_hdr->setStatus(STATUS_NOT_FOUND);
    WebHttpSetErrorResponse(whc, STATUS_NOT_FOUND);
    return WEB_HTTP_ERR_REQUEST_ERROR;
  }
  // get the file
  file_size = WebFileGetSize(h_file);
  file_date_gmt = WebFileGetDateGmt(h_file);
  request_file_ims = request->getModTime();

  // special logic for the autoconf port
  if ((whc->server_state & WEB_HTTP_SERVER_STATE_AUTOCONF) && (file_size == 0)) {
    response_hdr->setStatus(STATUS_NOT_FOUND);
    WebHttpSetErrorResponse(whc, STATUS_NOT_FOUND);
    WebFileClose(h_file);
    xfree(doc_root_file);
    return WEB_HTTP_ERR_REQUEST_ERROR;
  }
  // Check to see if the clients copy is up to date.  Ignore the
  // stupid content length that Netscape Navigator sends on the
  // If-Modified-Since line since it not in the HTTP 1.0 standard

  // Since the client sends If-Modified-Since in GMT, make sure that
  // we transform mtime to GMT
  if (request_file_ims != -1 && request_file_ims >= file_date_gmt) {
    response_hdr->setStatus(STATUS_NOT_MODIFIED);
  } else {
    // fetch the file from disk to memory
    response_hdr->setStatus(STATUS_OK);
    response_hdr->setLength(file_size);
    while (response_bdy->rawReadFromFile(h_file) > 0);
  }
  // set the document last-modified header
  response_hdr->setLastMod(file_date_gmt);

  WebFileClose(h_file);
  xfree(doc_root_file);

  return WEB_HTTP_ERR_OKAY;

}



//-------------------------------------------------------------------------
// read_request
//-------------------------------------------------------------------------

int
read_request(WebHttpContext * whc)
{

  const int buffer_size = 2048;
  char *buffer = (char *) alloca(buffer_size);

  httpMessage *request = whc->request;
  httpResponse *response_hdr = whc->response_hdr;

  // first get the request line
  if (sigfdrdln(whc->si, buffer, buffer_size) < 0) {
    // if we can not get the request line, update the status code so
    // it can get logged correctly but do not bother trying to send a
    // response
    response_hdr->setStatus(STATUS_BAD_REQUEST);
    return WEB_HTTP_ERR_REQUEST_FATAL;
  }


  if (request->addRequestLine(buffer) != 0) {
    response_hdr->setStatus(STATUS_BAD_REQUEST);
    WebHttpSetErrorResponse(whc, STATUS_BAD_REQUEST);
    return WEB_HTTP_ERR_REQUEST_ERROR;
  }
  // Check for a scheme we do not understand
  //
  //  If we undertand the scheme, it has
  //   to be HTTP
  if (request->getScheme() == SCHEME_UNKNOWN) {
    response_hdr->setStatus(STATUS_NOT_IMPLEMENTED);
    WebHttpSetErrorResponse(whc, STATUS_NOT_IMPLEMENTED);
    return WEB_HTTP_ERR_REQUEST_ERROR;
  }

  if (request->getMethod() != METHOD_GET && request->getMethod() != METHOD_POST && request->getMethod() != METHOD_HEAD) {
    response_hdr->setStatus(STATUS_NOT_IMPLEMENTED);
    WebHttpSetErrorResponse(whc, STATUS_NOT_IMPLEMENTED);
    return WEB_HTTP_ERR_REQUEST_ERROR;
  }
  // Read the headers of http request line by line until
  //   we get a line that is solely composed of "\r" (or
  //   just "" since not everyone follows the HTTP standard
  //
  do {
    if (sigfdrdln(whc->si, buffer, buffer_size) < 0) {
      response_hdr->setStatus(STATUS_BAD_REQUEST);
      return WEB_HTTP_ERR_REQUEST_FATAL;
    }
    request->addHeader(buffer);
  } while (strcmp(buffer, "\r") != 0 && *buffer != '\0');

  // If there is a content body, read it in
  if (request->addRequestBody(whc->si) < 0) {
    // There was error on reading the response body
    response_hdr->setStatus(STATUS_BAD_REQUEST);
    WebHttpSetErrorResponse(whc, STATUS_NOT_IMPLEMENTED);
    return WEB_HTTP_ERR_REQUEST_ERROR;
  }

  // Drain read channel: In the case of Linux, OS sends reset to the
  // socket if we close it when there is data left on it ot be read
  // (in compliance with TCP). This causes problems with the "POST"
  // method. (for example with update.html). With IE, we found ending
  // "\r\n" were not read.  The following work around is to read all
  // that is left in the socket before closing it.  The same problem
  // applies for Windows 2000 as well.
#if !defined(_WIN32)
#define MAX_DRAIN_BYTES 32
  // INKqa11524: If the user is malicious and keeps sending us data,
  // we'll go into an infinite spin here.  Fix is to only drain up
  // to 32 bytes to allow for funny browser behavior but to also
  // prevent reading forever.
  int drain_bytes = 0;
  if (fcntl(whc->si.fd, F_SETFL, O_NONBLOCK) >= 0) {
    char ch;
    while ((read(whc->si.fd, &ch, 1) > 0) && (drain_bytes < MAX_DRAIN_BYTES)) {
      drain_bytes++;
    }
  }
#else
  {
    unsigned long i;
    if (ioctlsocket(whc->si.fd, FIONREAD, &i) != SOCKET_ERROR) {
      if (i) {
        char *buf = (char *) alloca(i * sizeof(char));
        read_socket(whc->si.fd, buf, i);
      }
    }
  }
#endif

  return WEB_HTTP_ERR_OKAY;

}

//-------------------------------------------------------------------------
// write_response
//-------------------------------------------------------------------------

int
write_response(WebHttpContext * whc)
{
  char *buf_p;
  int bytes_to_write;
  int bytes_written;
  // Make sure that we have a content length
  if (whc->response_hdr->getLength() < 0) {
    whc->response_hdr->setLength(whc->response_bdy->spaceUsed());
  }
  whc->response_hdr->writeHdr(whc->si);
  if (whc->request->getMethod() != METHOD_HEAD) {
    buf_p = whc->response_bdy->bufPtr();
    bytes_to_write = whc->response_bdy->spaceUsed();
    bytes_written = 0;
    while (bytes_to_write) {
      bytes_written = socket_write(whc->si, buf_p, bytes_to_write);
      if (bytes_written < 0) {
        if (errno == EINTR || errno == EAGAIN)
          continue;
        else
          return WEB_HTTP_ERR_FAIL;
      } else {
        bytes_to_write -= bytes_written;
        buf_p += bytes_written;
      }
    }
  }
  return WEB_HTTP_ERR_OKAY;
}

//-------------------------------------------------------------------------
// process_query
//-------------------------------------------------------------------------

int
process_query(WebHttpContext * whc)
{
  int err;
  InkHashTable *ht;
  char *value;
  // processFormSubmission will substituteUnsafeChars()
  if ((ht = processFormSubmission((char *) (whc->request->getQuery()))) != NULL) {
    whc->query_data_ht = ht;
    // extract some basic info for easier access later
    if (ink_hash_table_lookup(ht, "mode", (void **) &value)) {
      if (strcmp(value, "1") == 0)
        whc->request_state |= WEB_HTTP_STATE_CONFIGURE;
    }
    if (ink_hash_table_lookup(ht, "detail", (void **) &value)) {
      if (strcmp(value, "more") == 0)
        whc->request_state |= WEB_HTTP_STATE_MORE_DETAIL;
    }
    err = WEB_HTTP_ERR_OKAY;
  } else {
    err = WEB_HTTP_ERR_FAIL;
  }
  return err;
}

//-------------------------------------------------------------------------
// process_post
//-------------------------------------------------------------------------

int
process_post(WebHttpContext * whc)
{
  int err;
  InkHashTable *ht;
  // processFormSubmission will substituteUnsafeChars()
  if ((ht = processFormSubmission(whc->request->getBody())) != NULL) {
    whc->post_data_ht = ht;
    err = WEB_HTTP_ERR_OKAY;
  } else {
    err = WEB_HTTP_ERR_FAIL;
  }
  return err;
}

//-------------------------------------------------------------------------
// signal_handler_init
//-------------------------------------------------------------------------

void
signal_handler_do_nothing(int x)
{
  //  A small function thats whole purpose is to give the signal
  //  handler for breaking out of a network read, somethng to call
  NOWARN_UNUSED(x);
}

int
signal_handler_init()
{
  // Setup signal handling.  We want to able to unstick stuck socket
  // connections.  This is accomplished by a watcher thread doing a
  // half close on the incoming socket after a timeout.  To break, out
  // the current read which is likely stuck we have a signal handler
  // on SIGUSR1 which does nothing except by side effect of break the
  // read.  All future reads from the socket should fail since
  // incoming traffic is shutdown on the connection and thread should
  // exit normally
#if !defined(_WIN32)
  sigset_t sigsToBlock;
  // FreeBSD and Linux use SIGUSR1 internally in the threads library
#if !defined(linux) && !defined(freebsd) && !defined(darwin)
  // Set up the handler for SIGUSR1
  struct sigaction sigHandler;
  sigHandler.sa_handler = signal_handler_do_nothing;
  sigemptyset(&sigHandler.sa_mask);
  sigHandler.sa_flags = 0;
  sigaction(SIGUSR1, &sigHandler, NULL);
#endif
  // Block all other signals
  sigfillset(&sigsToBlock);
  sigdelset(&sigsToBlock, SIGUSR1);
  ink_thread_sigsetmask(SIG_SETMASK, &sigsToBlock, NULL);
#endif // !_WIN32
  return WEB_HTTP_ERR_OKAY;
}

//-------------------------------------------------------------------------
// ssl_init
//-------------------------------------------------------------------------

int
ssl_init(WebHttpContext * whc)
{
  SSL *SSL_con = NULL;
  unsigned int sslErrno;
  char ssl_Error[256];
  SSL_con = SSL_new(whc->ssl_ctx);
  SSL_set_fd(SSL_con, whc->si.fd);
  if (SSL_accept(SSL_con) < 0) {
    sslErrno = ERR_get_error();
    ERR_error_string(sslErrno, ssl_Error);
    mgmt_log(stderr, "[ssl_init] SSL_accept failed: %s", ssl_Error);
    return WEB_HTTP_ERR_FAIL;
  }
  whc->si.SSLcon = SSL_con;

  return WEB_HTTP_ERR_OKAY;
}

//-------------------------------------------------------------------------
// ssl_free
//-------------------------------------------------------------------------

int
ssl_free(WebHttpContext * whc)
{
  if (whc->si.SSLcon != NULL) {
    SSL_free((SSL *) whc->si.SSLcon);
  }

  return WEB_HTTP_ERR_OKAY;
}

//-------------------------------------------------------------------------
// WebHttpInit
//-------------------------------------------------------------------------

void
WebHttpInit()
{

  static int initialized = 0;

  if (initialized != 0) {
    mgmt_log(stderr, "[WebHttpInit] error, initialized twice (%d)", initialized);
  }
  initialized++;

  // initialize autoconf allow files
  g_autoconf_allow_ht = ink_hash_table_create(InkHashTableKeyType_String);
  ink_hash_table_insert(g_autoconf_allow_ht, "/proxy.pac", NULL);
  ink_hash_table_insert(g_autoconf_allow_ht, "/wpad.dat", NULL);
  ink_hash_table_insert(g_autoconf_allow_ht, "/public_key.der", NULL);
  ink_hash_table_insert(g_autoconf_allow_ht, "/synthetic.txt", NULL);

  // initialize submit bindings
  g_submit_bindings_ht = ink_hash_table_create(InkHashTableKeyType_String);
  ink_hash_table_insert(g_submit_bindings_ht, HTML_SUBMIT_ALARM_FILE, (void *) handle_submit_alarm);
  ink_hash_table_insert(g_submit_bindings_ht, HTML_SUBMIT_MGMT_AUTH_FILE, (void *) handle_submit_mgmt_auth);
  //ink_hash_table_insert(g_submit_bindings_ht, HTML_SUBMIT_SNAPSHOT_FILE, handle_submit_snapshot);
  ink_hash_table_insert(g_submit_bindings_ht, HTML_SUBMIT_VIEW_LOGS_FILE, (void *) handle_submit_view_logs);
  // initialize file bindings
  g_file_bindings_ht = ink_hash_table_create(InkHashTableKeyType_String);
  ink_hash_table_insert(g_file_bindings_ht, HTML_CHART_FILE, (void *) handle_chart);
  ink_hash_table_insert(g_file_bindings_ht, HTML_BACKDOOR_CONFIG_FILES, (void *) handle_config_files);
  ink_hash_table_insert(g_file_bindings_ht, HTML_BACKDOOR_DEBUG_LOGS, (void *) handle_debug_logs);
  ink_hash_table_insert(g_file_bindings_ht, HTML_SYNTHETIC_FILE, (void *) handle_synthetic);

  // initialize extension bindings
  g_extn_bindings_ht = ink_hash_table_create(InkHashTableKeyType_String);
  ink_hash_table_insert(g_extn_bindings_ht, ".cgi", (void *) handle_cgi_extn);
  ink_hash_table_insert(g_extn_bindings_ht, ".ink", (void *) handle_ink_extn);

  // initialize the configurator editing bindings which binds
  // configurator display filename (eg. f_cache_config.ink) to
  // its mgmt API config file type (TSFileNameT)
  g_display_config_ht = ink_hash_table_create(InkHashTableKeyType_String);
  ink_hash_table_insert(g_display_config_ht, HTML_FILE_CACHE_CONFIG, (void *) TS_FNAME_CACHE_OBJ);
  ink_hash_table_insert(g_display_config_ht, HTML_FILE_HOSTING_CONFIG, (void *) TS_FNAME_HOSTING);
  ink_hash_table_insert(g_display_config_ht, HTML_FILE_ICP_CONFIG, (void *) TS_FNAME_ICP_PEER);
  ink_hash_table_insert(g_display_config_ht, HTML_FILE_IP_ALLOW_CONFIG, (void *) TS_FNAME_IP_ALLOW);
  ink_hash_table_insert(g_display_config_ht, HTML_FILE_MGMT_ALLOW_CONFIG, (void *) TS_FNAME_MGMT_ALLOW);
  ink_hash_table_insert(g_display_config_ht, HTML_FILE_PARENT_CONFIG, (void *) TS_FNAME_PARENT_PROXY);
  ink_hash_table_insert(g_display_config_ht, HTML_FILE_PARTITION_CONFIG, (void *) TS_FNAME_PARTITION);
  ink_hash_table_insert(g_display_config_ht, HTML_FILE_REMAP_CONFIG, (void *) TS_FNAME_REMAP);
  ink_hash_table_insert(g_display_config_ht, HTML_FILE_SOCKS_CONFIG, (void *) TS_FNAME_SOCKS);
  ink_hash_table_insert(g_display_config_ht, HTML_FILE_SPLIT_DNS_CONFIG, (void *) TS_FNAME_SPLIT_DNS);
  ink_hash_table_insert(g_display_config_ht, HTML_FILE_UPDATE_CONFIG, (void *) TS_FNAME_UPDATE_URL);
  ink_hash_table_insert(g_display_config_ht, HTML_FILE_VADDRS_CONFIG, (void *) TS_FNAME_VADDRS);

  // initialize other modules
  WebHttpAuthInit();
  WebHttpLogInit();
  WebHttpSessionInit();
#if TS_HAS_WEBUI
  WebHttpRenderInit();
#endif

  return;
}

//-------------------------------------------------------------------------
// WebHttpHandleConnection
//
// Handles http requests across the web management port
//-------------------------------------------------------------------------

void
WebHttpHandleConnection(WebHttpConInfo * whci)
{

  int err = WEB_HTTP_ERR_OKAY;

  WebHttpContext *whc;
  WebHttpHandler handler;
  char *file;
  char *extn;
  int drain_bytes;
  char ch;

  // initialization
  if ((whc = WebHttpContextCreate(whci)) == NULL)
    goto Ltransaction_close;
  if (signal_handler_init() != WEB_HTTP_ERR_OKAY)
    goto Ltransaction_close;
  if (whc->server_state & WEB_HTTP_SERVER_STATE_SSL_ENABLED)
    if (ssl_init(whc) != WEB_HTTP_ERR_OKAY)
      goto Ltransaction_close;

  // read request
  if ((err = read_request(whc)) != WEB_HTTP_ERR_OKAY)
    goto Lerror_switch;

  // authentication
  if (whc->server_state & WEB_HTTP_SERVER_STATE_AUTH_ENABLED)
    if (WebHttpAuthenticate(whc) != WEB_HTTP_ERR_OKAY)
      goto Ltransaction_send;

  // get our file information
  file = (char *) (whc->request->getFile());
  if (strcmp("/", file) == 0) {
    file = (char *) (whc->default_file);
  }

  Debug("web2", "[WebHttpHandleConnection] request file: %s", file);


  if (whc->server_state & WEB_HTTP_SERVER_STATE_AUTOCONF) {

    // security concern: special treatment if we're handling a request
    // on the autoconf port.  can't have users downloading arbitrary
    // files under the config directory!
    if (!ink_hash_table_isbound(g_autoconf_allow_ht, file)) {
      mgmt_elog(stderr,"[WebHttpHandleConnection] %s not valid autoconf file",file);
      whc->response_hdr->setStatus(STATUS_NOT_FOUND);
      WebHttpSetErrorResponse(whc, STATUS_NOT_FOUND);
      goto Ltransaction_send;
    }
  } else {
#if TS_HAS_WEBUI
    if (WebHttpTreeReturnRefresh(file)) {
      // if we are handling a monitor/mrtg page, configure it to refresh
      if (strncmp(file, "/monitor/", 9) == 0) {
        whc->response_hdr->setRefresh(wGlobals.refreshRate);
      } else if (strncmp(file, "/mrtg/", 6) == 0) {
        whc->response_hdr->setRefresh(REFRESH_RATE_MRTG);
      } else {                  // default
        whc->response_hdr->setRefresh(wGlobals.refreshRate);
      }
    }
#endif
    // Make a note if we are a plugin.  Being a plugin will affect our
    // doc_root and how request files and doc_roots are joined to
    // generate an absolute path.  See WebHttpAddDocRoot_Xmalloc()
    if (strncmp(file, "/plugins/", 9) == 0) {
      whc->request_state |= WEB_HTTP_STATE_PLUGIN;
    }

  }

  // process query
  process_query(whc);

  // check submit_binding;
  // if nothing, check file_binding;
  // if nothing, check extn_binding;
  // if still nothing, use the default handler;
  if (ink_hash_table_lookup(g_submit_bindings_ht, file, (void **) &handler)) {
    // workaround: sometimes we receive a GET for our submit cgi's
    // (rather than a resubmitted POST).  In this case, just render
    // the default page since we can't do much else
    if (whc->request->getMethod() != METHOD_POST) {
      if ((strcmp(file, HTML_SUBMIT_INSPECTOR_DPY_FILE) != 0) && (strcmp(file, HTML_SUBMIT_CONFIG_DISPLAY) != 0)) {
        err = WebHttpRender(whc, HTML_DEFAULT_MONITOR_FILE);
        goto Lerror_switch;
      }
    }
    // process post
    process_post(whc);
    // only allow one submission at a time
    ink_mutex_acquire(&wGlobals.submitLock);
    err = handler(whc, file);
    ink_mutex_release(&wGlobals.submitLock);
  } else {
    if (!ink_hash_table_lookup(g_file_bindings_ht, file, (void **) &handler)) {
      extn = file;
      while (*extn != '\0')
        extn++;
      while ((extn > file) && (*extn != '.'))
        extn--;
      if (!ink_hash_table_lookup(g_extn_bindings_ht, extn, (void **) &handler)) {
        handler = handle_default;
      }
    }
    err = handler(whc, file);
  }

Lerror_switch:

  switch (err) {
  case WEB_HTTP_ERR_OKAY:
  case WEB_HTTP_ERR_REQUEST_ERROR:
    goto Ltransaction_send;
  case WEB_HTTP_ERR_FAIL:
  case WEB_HTTP_ERR_REQUEST_FATAL:
  default:
    goto Ltransaction_close;
  }

Ltransaction_send:

  // write response
  if ((err = write_response(whc)) != WEB_HTTP_ERR_OKAY)
    goto Ltransaction_close;

  // close the connection before logging it to reduce latency
#ifndef _WIN32
  shutdown(whc->si.fd, 1);
  drain_bytes = 0;
  if (fcntl(whc->si.fd, F_SETFL, O_NONBLOCK) >= 0) {
    while ((read(whc->si.fd, &ch, 1) > 0) && (drain_bytes < MAX_DRAIN_BYTES)) {
      drain_bytes++;
    }
  }
#endif
  close_socket(whc->si.fd);
  whc->si.fd = -1;

  // log transaction
  if (wGlobals.logFD >= 0)
    WebHttpLogTransaction(whc);

Ltransaction_close:

  // if we didn't close already, close connection
  if (whc->si.fd != -1) {
#ifndef _WIN32
    shutdown(whc->si.fd, 1);
    drain_bytes = 0;
    if (fcntl(whc->si.fd, F_SETFL, O_NONBLOCK) >= 0) {
      while ((read(whc->si.fd, &ch, 1) > 0) && (drain_bytes < MAX_DRAIN_BYTES)) {
        drain_bytes++;
      }
    }
#endif
    close_socket(whc->si.fd);
  }
  // clean up ssl
  if (whc->server_state & WEB_HTTP_SERVER_STATE_SSL_ENABLED)
    ssl_free(whc);

  // clean up memory
  WebHttpContextDestroy(whc);

  return;

}

//-------------------------------------------------------------------------
// WebHttpSetErrorResponse
//
// Formulates a page to return on an HttpStatus condition
//-------------------------------------------------------------------------

void
WebHttpSetErrorResponse(WebHttpContext * whc, HttpStatus_t error)
{

  //-----------------------------------------------------------------------
  // FIXME: HARD-CODED HTML HELL!!!
  //-----------------------------------------------------------------------

  static const char a[] = "<HTML>\n<Head>\n<TITLE>";
  static const char b[] = "</TITLE>\n</HEAD>\n<BODY bgcolor=\"#FFFFFF\"><h1>\n";
  static const char c[] = "</h1>\n</BODY>\n</HTML>\n";
  int errorMsgLen = strlen(httpStatStr[error]);

  // reset the buffer
  whc->response_bdy->reUse();

  // fill in the buffer
  whc->response_bdy->copyFrom(a, strlen(a));
  whc->response_bdy->copyFrom(httpStatStr[error], errorMsgLen);
  whc->response_bdy->copyFrom(b, strlen(b));
  whc->response_bdy->copyFrom(httpStatStr[error], errorMsgLen);
  whc->response_bdy->copyFrom(c, strlen(c));

}

//-------------------------------------------------------------------------
// WebHttpAddDocRoot_Xmalloc
//-------------------------------------------------------------------------

char *
WebHttpAddDocRoot_Xmalloc(WebHttpContext * whc, const char *file)
{

  int file_len = 0;
  char *doc_root_file;
  bool is_plugin;

  is_plugin = whc->request_state & WEB_HTTP_STATE_PLUGIN;

  file_len = strlen(file);
  file_len += is_plugin ? strlen(whc->plugin_doc_root) : strlen(whc->doc_root);
  doc_root_file = (char *) xmalloc(file_len + 1);

  if (is_plugin) {
    ink_strncpy(doc_root_file, whc->plugin_doc_root, file_len);
    strncat(doc_root_file, file + strlen("/plugins"), file_len - strlen(doc_root_file));
  } else {
    ink_strncpy(doc_root_file, whc->doc_root, file_len);
    strncat(doc_root_file, file, file_len - strlen(doc_root_file));
  }

  return doc_root_file;

}