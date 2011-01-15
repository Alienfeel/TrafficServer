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

#include "UrlRewrite.h"
#include "Main.h"
#include "Error.h"
#include "P_EventSystem.h"
#include "StatSystem.h"
#include "P_Cache.h"
#include "ProxyConfig.h"
#include "ReverseProxy.h"
#include "MatcherUtils.h"
#include "Tokenizer.h"
#include "api/ts/remap.h"
#include "UrlMappingPathIndex.h"

#include "ink_string.h"


unsigned long
check_remap_option(char *argv[], int argc, unsigned long findmode = 0, int *_ret_idx = NULL, char **argptr = NULL)
{
  unsigned long ret_flags = 0;
  int idx = 0;

  if (argptr)
    *argptr = NULL;
  if (argv && argc > 0) {
    for (int i = 0; i < argc; i++) {
      if (!ink_string_fast_strcasecmp(argv[i], "map_with_referer")) {
        if ((findmode & REMAP_OPTFLG_MAP_WITH_REFERER) != 0)
          idx = i;
        ret_flags |= REMAP_OPTFLG_MAP_WITH_REFERER;
      } else if (!ink_string_fast_strncasecmp(argv[i], "plugin=", 7)) {
        if ((findmode & REMAP_OPTFLG_PLUGIN) != 0)
          idx = i;
        if (argptr)
          *argptr = &argv[i][7];
        ret_flags |= REMAP_OPTFLG_PLUGIN;
      } else if (!ink_string_fast_strncasecmp(argv[i], "pparam=", 7)) {
        if ((findmode & REMAP_OPTFLG_PPARAM) != 0)
          idx = i;
        if (argptr)
          *argptr = &argv[i][7];
        ret_flags |= REMAP_OPTFLG_PPARAM;
      } else if (!ink_string_fast_strncasecmp(argv[i], "method=", 7)) {
        if ((findmode & REMAP_OPTFLG_METHOD) != 0)
          idx = i;
        if (argptr)
          *argptr = &argv[i][7];
        ret_flags |= REMAP_OPTFLG_METHOD;
      } else if (!ink_string_fast_strncasecmp(argv[i], "src_ip=~", 8)) {
        if ((findmode & REMAP_OPTFLG_SRC_IP) != 0)
          idx = i;
        if (argptr)
          *argptr = &argv[i][8];
        ret_flags |= (REMAP_OPTFLG_SRC_IP | REMAP_OPTFLG_INVERT);
      } else if (!ink_string_fast_strncasecmp(argv[i], "src_ip=", 7)) {
        if ((findmode & REMAP_OPTFLG_SRC_IP) != 0)
          idx = i;
        if (argptr)
          *argptr = &argv[i][7];
        ret_flags |= REMAP_OPTFLG_SRC_IP;
      } else if (!ink_string_fast_strncasecmp(argv[i], "action=", 7)) {
        if ((findmode & REMAP_OPTFLG_ACTION) != 0)
          idx = i;
        if (argptr)
          *argptr = &argv[i][7];
        ret_flags |= REMAP_OPTFLG_ACTION;
      } else if (!ink_string_fast_strncasecmp(argv[i], "mapid=", 6)) {
        if ((findmode & REMAP_OPTFLG_MAP_ID) != 0)
          idx = i;
        if (argptr)
          *argptr = &argv[i][6];
        ret_flags |= REMAP_OPTFLG_MAP_ID;
      }


      if ((findmode & ret_flags) && !argptr) {
        if (_ret_idx)
          *_ret_idx = idx;
        return ret_flags;
      }

    }
  }
  if (_ret_idx)
    *_ret_idx = idx;
  return ret_flags;
}

/**
  Determines where we are in a situation where a virtual path is
  being mapped to a server home page. If it is, we set a special flag
  instructing us to be on the lookout for the need to send a redirect
  to if the request URL is a object, opposed to a directory. We need
  the redirect for an object so that the browser is aware that it is
  real accessing a directory (albeit a virtual one).

*/
void
SetHomePageRedirectFlag(url_mapping * new_mapping, URL &new_to_url)
{
  int fromLen, toLen;
  const char *from_path = new_mapping->fromURL.path_get(&fromLen);
  const char *to_path = new_to_url.path_get(&toLen);

  new_mapping->homePageRedirect = (from_path && !to_path) ? true : false;
}


// ===============================================================================
static int
is_inkeylist(char *key, ...)
{
  int i, idx, retcode = 0;

  if (likely(key && key[0])) {
    char tmpkey[512];
    va_list ap;
    va_start(ap, key);
    for (i = 0; i < (int) (sizeof(tmpkey) - 2) && (tmpkey[i] = *key++) != 0;)
      if (tmpkey[i] != '_' && tmpkey[i] != '.')
        i++;
    tmpkey[i] = 0;

    if (tmpkey[0]) {
      char *str = va_arg(ap, char *);
      for (idx = 1; str; idx++) {
        if (!strcasecmp(tmpkey, str)) {
          retcode = idx;
          break;
        }
        str = va_arg(ap, char *);
      }
    }
    va_end(ap);
  }
  return retcode;
}

/**
  Cleanup *char[] array - each item in array must be allocated via
  xmalloc or similar "x..." function.

*/
static void
clear_xstr_array(char *v[], int vsize)
{
  if (v && vsize > 0) {
    for (int i = 0; i < vsize; i++) {
      if (v[i]) {
        v[i] = (char *) xfree_null(v[i]);
      }
    }
  }
}

static const char *
validate_filter_args(acl_filter_rule ** rule_pp, char **argv, int argc, char *errStrBuf, int errStrBufSize)
{
  acl_filter_rule *rule;
  unsigned long ul;
  char *argptr, tmpbuf[1024];
  SRC_IP_INFO *ipi;
  int i, j, m;
  bool new_rule_flg = false;

  if (!rule_pp) {
    Debug("url_rewrite", "[validate_filter_args] Invalid argument(s)");
    return (const char *) "Invalid argument(s)";
  }

  if (is_debug_tag_set("url_rewrite")) {
    printf("validate_filter_args: ");
    for (i = 0; i < argc; i++)
      printf("\"%s\" ", argv[i]);
    printf("\n");
  }

  if ((rule = *rule_pp) == NULL) {
    rule = NEW(new acl_filter_rule());
    if (unlikely((*rule_pp = rule) == NULL)) {
      Debug("url_rewrite", "[validate_filter_args] Memory allocation error");
      return (const char *) "Memory allocation Error";
    }
    new_rule_flg = true;
    Debug("url_rewrite", "[validate_filter_args] new acl_filter_rule class was created during remap rule processing");
  }

  for (i = 0; i < argc; i++) {
    if ((ul = check_remap_option(&argv[i], 1, 0, NULL, &argptr)) == 0) {
      Debug("url_rewrite", "[validate_filter_args] Unknow remap option - %s", argv[i]);
      snprintf(errStrBuf, errStrBufSize, "Unknown option - \"%s\"", argv[i]);
      errStrBuf[errStrBufSize - 1] = 0;
      if (new_rule_flg) {
        delete rule;
        *rule_pp = NULL;
      }
      return (const char *) errStrBuf;
    }
    if (!argptr || !argptr[0]) {
      Debug("url_rewrite", "[validate_filter_args] Empty argument in %s", argv[i]);
      snprintf(errStrBuf, errStrBufSize, "Empty argument in \"%s\"", argv[i]);
      errStrBuf[errStrBufSize - 1] = 0;
      if (new_rule_flg) {
        delete rule;
        *rule_pp = NULL;
      }
      return (const char *) errStrBuf;
    }

    if (ul & REMAP_OPTFLG_METHOD) {     /* "method=" option */
      if (rule->method_cnt >= ACL_FILTER_MAX_METHODS) {
        Debug("url_rewrite", "[validate_filter_args] Too many \"method=\" filters");
        snprintf(errStrBuf, errStrBufSize, "Defined more than %d \"method=\" filters!", ACL_FILTER_MAX_METHODS);
        errStrBuf[errStrBufSize - 1] = 0;
        if (new_rule_flg) {
          delete rule;
          *rule_pp = NULL;
        }
        return (const char *) errStrBuf;
      }
      // Please remember that the order of hash idx creation is very important and it is defined
      // in HTTP.cc file
      if (!ink_string_fast_strcasecmp(argptr, "CONNECT"))
        m = HTTP_WKSIDX_CONNECT;
      else if (!ink_string_fast_strcasecmp(argptr, "DELETE"))
        m = HTTP_WKSIDX_DELETE;
      else if (!ink_string_fast_strcasecmp(argptr, "GET"))
        m = HTTP_WKSIDX_GET;
      else if (!ink_string_fast_strcasecmp(argptr, "HEAD"))
        m = HTTP_WKSIDX_HEAD;
      else if (!ink_string_fast_strcasecmp(argptr, "ICP_QUERY"))
        m = HTTP_WKSIDX_ICP_QUERY;
      else if (!ink_string_fast_strcasecmp(argptr, "OPTIONS"))
        m = HTTP_WKSIDX_OPTIONS;
      else if (!ink_string_fast_strcasecmp(argptr, "POST"))
        m = HTTP_WKSIDX_POST;
      else if (!ink_string_fast_strcasecmp(argptr, "PURGE"))
        m = HTTP_WKSIDX_PURGE;
      else if (!ink_string_fast_strcasecmp(argptr, "PUT"))
        m = HTTP_WKSIDX_PUT;
      else if (!ink_string_fast_strcasecmp(argptr, "TRACE"))
        m = HTTP_WKSIDX_TRACE;
      else if (!ink_string_fast_strcasecmp(argptr, "PUSH"))
        m = HTTP_WKSIDX_PUSH;
      else {
        Debug("url_rewrite", "[validate_filter_args] Unknown method value %s", argptr);
        snprintf(errStrBuf, errStrBufSize, "Unknown method \"%s\"", argptr);
        errStrBuf[errStrBufSize - 1] = 0;
        if (new_rule_flg) {
          delete rule;
          *rule_pp = NULL;
        }
        return (const char *) errStrBuf;
      }
      for (j = 0; j < rule->method_cnt; j++) {
        if (rule->method_array[j] == m) {
          m = 0;
          break;                /* we already have it in the list */
        }
      }
      if ((j = m) != 0) {
        j = j - HTTP_WKSIDX_CONNECT;    // get method index
        if (j<0 || j>= ACL_FILTER_MAX_METHODS) {
          Debug("url_rewrite", "[validate_filter_args] Incorrect method index! Method sequence in HTTP.cc is broken");
          snprintf(errStrBuf, errStrBufSize, "Incorrect method index %d", j);
          errStrBuf[errStrBufSize - 1] = 0;
          if (new_rule_flg) {
            delete rule;
            *rule_pp = NULL;
          }
          return (const char *) errStrBuf;
        }
        rule->method_idx[j] = m;
        rule->method_array[rule->method_cnt++] = m;
        rule->method_valid = 1;
      }
    } else if (ul & REMAP_OPTFLG_SRC_IP) {      /* "src_ip=" option */
      if (rule->src_ip_cnt >= ACL_FILTER_MAX_SRC_IP) {
        Debug("url_rewrite", "[validate_filter_args] Too many \"src_ip=\" filters");
        snprintf(errStrBuf, errStrBufSize, "Defined more than %d \"src_ip=\" filters!", ACL_FILTER_MAX_SRC_IP);
        errStrBuf[errStrBufSize - 1] = 0;
        if (new_rule_flg) {
          delete rule;
          *rule_pp = NULL;
        }
        return (const char *) errStrBuf;
      }
      ipi = &rule->src_ip_array[rule->src_ip_cnt];
      if (ul & REMAP_OPTFLG_INVERT)
        ipi->invert = true;
      strncpy(tmpbuf, argptr, sizeof(tmpbuf) - 1);
      tmpbuf[sizeof(tmpbuf) - 1] = 0; // important! use copy of argument
      if (ExtractIpRange(tmpbuf, (unsigned long*) &ipi->start, &ipi->end) != NULL) {
        Debug("url_rewrite", "[validate_filter_args] Unable to parse IP value in %s", argv[i]);
        snprintf(errStrBuf, errStrBufSize, "Unable to parse IP value in %s", argv[i]);
        errStrBuf[errStrBufSize - 1] = 0;
        if (new_rule_flg) {
          delete rule;
          *rule_pp = NULL;
        }
        return (const char*) errStrBuf;
      }
      for (j = 0; j < rule->src_ip_cnt; j++) {
        if (rule->src_ip_array[j].start == ipi->start && rule->src_ip_array[j].end == ipi->end) {
          ipi->reset();
          ipi = NULL;
          break;                /* we have the same src_ip in the list */
        }
      }
      if (ipi) {
        rule->src_ip_cnt++;
        rule->src_ip_valid = 1;
      }
    } else if (ul & REMAP_OPTFLG_ACTION) {      /* "action=" option */
      if (is_inkeylist(argptr, "0", "off", "deny", "disable", NULL)) {
        rule->allow_flag = 0;
      } else if (is_inkeylist(argptr, "1", "on", "allow", "enable", NULL)) {
        rule->allow_flag = 1;
      } else {
        Debug("url_rewrite", "[validate_filter_args] Unknown argument \"%s\"", argv[i]);
        snprintf(errStrBuf, errStrBufSize, "Unknown argument \"%s\"", argv[i]);
        errStrBuf[errStrBufSize - 1] = 0;
        if (new_rule_flg) {
          delete rule;
          *rule_pp = NULL;
        }
        return (const char *) errStrBuf;
      }
    }
  }

  if (is_debug_tag_set("url_rewrite"))
    rule->print();

  return NULL;                  /* success */
}


static const char *
parse_directive(BUILD_TABLE_INFO * bti, char *errbuf, int errbufsize)
{
  bool flg;
  char *directive = NULL;
  acl_filter_rule *rp, **rpp;
  const char *cstr = NULL;

  // Check arguments
  if (unlikely(!bti || !errbuf || errbufsize <= 0 || !bti->paramc || (directive = bti->paramv[0]) == NULL)) {
    Debug("url_rewrite", "[parse_directive] Invalid argument(s)");
    return "Invalid argument(s)";
  }

  Debug("url_rewrite", "[parse_directive] Start processing \"%s\" directive", directive);

  if (directive[0] != '.' || directive[1] == 0) {
    snprintf(errbuf, errbufsize, "Invalid directive \"%s\"", directive);
    Debug("url_rewrite", "[parse_directive] %s", errbuf);
    return (const char *) errbuf;
  }
  if (is_inkeylist(&directive[1], "definefilter", "deffilter", "defflt", NULL)) {
    if (bti->paramc < 2) {
      snprintf(errbuf, errbufsize, "Directive \"%s\" must have name argument", directive);
      Debug("url_rewrite", "[parse_directive] %s", errbuf);
      return (const char *) errbuf;
    }
    if (bti->argc < 1) {
      snprintf(errbuf, errbufsize, "Directive \"%s\" must have filter parameter(s)", directive);
      Debug("url_rewrite", "[parse_directive] %s", errbuf);
      return (const char *) errbuf;
    }

    flg = ((rp = acl_filter_rule::find_byname(bti->rules_list, (const char *) bti->paramv[1])) == NULL) ? true : false;
    // coverity[alloc_arg]
    if ((cstr = validate_filter_args(&rp, bti->argv, bti->argc, errbuf, errbufsize)) == NULL && rp) {
      if (flg)                  // new filter - add to list
      {
        Debug("url_rewrite", "[parse_directive] new rule \"%s\" was created", bti->paramv[1]);
        for (rpp = &bti->rules_list; *rpp; rpp = &((*rpp)->next));
        (*rpp = rp)->name(bti->paramv[1]);
      }
      Debug("url_rewrite", "[parse_directive] %d argument(s) were added to rule \"%s\"", bti->argc, bti->paramv[1]);
      rp->add_argv(bti->argc, bti->argv);       // store string arguments for future processing
    }
  } else if (is_inkeylist(&directive[1], "deletefilter", "delfilter", "delflt", NULL)) {
    if (bti->paramc < 2) {
      snprintf(errbuf, errbufsize, "Directive \"%s\" must have name argument", directive);
      Debug("url_rewrite", "[parse_directive] %s", errbuf);
      return (const char *) errbuf;
    }
    acl_filter_rule::delete_byname(&bti->rules_list, (const char *) bti->paramv[1]);
  } else if (is_inkeylist(&directive[1], "usefilter", "activefilter", "activatefilter", "useflt", NULL)) {
    if (bti->paramc < 2) {
      snprintf(errbuf, errbufsize, "Directive \"%s\" must have name argument", directive);
      Debug("url_rewrite", "[parse_directive] %s", errbuf);
      return (const char *) errbuf;
    }
    if ((rp = acl_filter_rule::find_byname(bti->rules_list, (const char *) bti->paramv[1])) == NULL) {
      snprintf(errbuf, errbufsize, "Undefined filter \"%s\" in directive \"%s\"", bti->paramv[1], directive);
      Debug("url_rewrite", "[parse_directive] %s", errbuf);
      return (const char *) errbuf;
    }
    acl_filter_rule::requeue_in_active_list(&bti->rules_list, rp);
  } else
    if (is_inkeylist(&directive[1], "unusefilter", "deactivatefilter", "unactivefilter", "deuseflt", "unuseflt", NULL))
  {
    if (bti->paramc < 2) {
      snprintf(errbuf, errbufsize, "Directive \"%s\" must have name argument", directive);
      Debug("url_rewrite", "[parse_directive] %s", errbuf);
      return (const char *) errbuf;
    }
    if ((rp = acl_filter_rule::find_byname(bti->rules_list, (const char *) bti->paramv[1])) == NULL) {
      snprintf(errbuf, errbufsize, "Undefined filter \"%s\" in directive \"%s\"", bti->paramv[1], directive);
      Debug("url_rewrite", "[parse_directive] %s", errbuf);
      return (const char *) errbuf;
    }
    acl_filter_rule::requeue_in_passive_list(&bti->rules_list, rp);
  } else {
    snprintf(errbuf, errbufsize, "Unknown directive \"%s\"", directive);
    Debug("url_rewrite", "[parse_directive] %s", errbuf);
    return (const char *) errbuf;
  }
  return cstr;
}


static const char *
process_filter_opt(url_mapping * mp, BUILD_TABLE_INFO * bti, char *errStrBuf, int errStrBufSize)
{
  acl_filter_rule *rp, **rpp;
  const char *errStr = NULL;

  if (unlikely(!mp || !bti || !errStrBuf || errStrBufSize <= 0)) {
    Debug("url_rewrite", "[process_filter_opt] Invalid argument(s)");
    return (const char *) "[process_filter_opt] Invalid argument(s)";
  }
  for (rp = bti->rules_list; rp; rp = rp->next) {
    if (rp->active_queue_flag) {
      Debug("url_rewrite", "[process_filter_opt] Add active main filter \"%s\" (argc=%d)",
            rp->filter_name ? rp->filter_name : "<NULL>", rp->argc);
      for (rpp = &mp->filter; *rpp; rpp = &((*rpp)->next));
      if ((errStr = validate_filter_args(rpp, rp->argv, rp->argc, errStrBuf, errStrBufSize)) != NULL)
        break;
    }
  }
  if (!errStr && (bti->remap_optflg & REMAP_OPTFLG_ALL_FILTERS) != 0) {
    Debug("url_rewrite", "[process_filter_opt] Add per remap filter");
    for (rpp = &mp->filter; *rpp; rpp = &((*rpp)->next));
    errStr = validate_filter_args(rpp, bti->argv, bti->argc, errStrBuf, errStrBufSize);
  }
  return errStr;
}

//
// CTOR / DTOR for the UrlRewrite class.
//
UrlRewrite::UrlRewrite(const char *file_var_in)
 : nohost_rules(0), reverse_proxy(0), backdoor_enabled(0),
   mgmt_autoconf_port(0), default_to_pac(0), default_to_pac_port(0), file_var(NULL), ts_name(NULL),
   http_default_redirect_url(NULL), num_rules_forward(0), num_rules_reverse(0), num_rules_redirect_permanent(0),
   num_rules_redirect_temporary(0), remap_pi_list(NULL)
{

  forward_mappings.hash_lookup = reverse_mappings.hash_lookup =
    permanent_redirects.hash_lookup = temporary_redirects.hash_lookup = NULL;

  char *config_file = NULL;

  ink_assert(file_var_in != NULL);
  this->file_var = xstrdup(file_var_in);
  config_file_path[0] = '\0';

  REVERSE_ReadConfigStringAlloc(config_file, file_var_in);

  if (config_file == NULL) {
    pmgmt->signalManager(MGMT_SIGNAL_CONFIG_ERROR, "Unable to find proxy.config.url_remap.filename");
    Warning("%s Unable to locate remap.config.  No remappings in effect", modulePrefix);
    return;
  }

  this->ts_name = NULL;
  REVERSE_ReadConfigStringAlloc(this->ts_name, "proxy.config.proxy_name");
  if (this->ts_name == NULL) {
    pmgmt->signalManager(MGMT_SIGNAL_CONFIG_ERROR, "Unable to read proxy.config.proxy_name");
    Warning("%s Unable to determine proxy name.  Incorrect redirects could be generated", modulePrefix);
    this->ts_name = xstrdup("");
  }

  this->http_default_redirect_url = NULL;
  REVERSE_ReadConfigStringAlloc(this->http_default_redirect_url, "proxy.config.http.referer_default_redirect");
  if (this->http_default_redirect_url == NULL) {
    pmgmt->signalManager(MGMT_SIGNAL_CONFIG_ERROR, "Unable to read proxy.config.http.referer_default_redirect");
    Warning("%s Unable to determine default redirect url for \"referer\" filter.", modulePrefix);
    this->http_default_redirect_url = xstrdup("http://www.apache.org");
  }

  REVERSE_ReadConfigInteger(reverse_proxy, "proxy.config.reverse_proxy.enabled");
  REVERSE_ReadConfigInteger(mgmt_autoconf_port, "proxy.config.admin.autoconf_port");
  REVERSE_ReadConfigInteger(default_to_pac, "proxy.config.url_remap.default_to_server_pac");
  REVERSE_ReadConfigInteger(default_to_pac_port, "proxy.config.url_remap.default_to_server_pac_port");
  REVERSE_ReadConfigInteger(url_remap_mode, "proxy.config.url_remap.url_remap_mode");
  REVERSE_ReadConfigInteger(backdoor_enabled, "proxy.config.url_remap.handle_backdoor_urls");

  ink_strncpy(config_file_path, system_config_directory, sizeof(config_file_path));
  strncat(config_file_path, "/", sizeof(config_file_path) - strlen(config_file_path) - 1);
  strncat(config_file_path, config_file, sizeof(config_file_path) - strlen(config_file_path) - 1);
  xfree(config_file);

  if (this->BuildTable() != 0) {
    Warning("something failed during BuildTable() -- check your remap plugins!");
  }

  pcre_malloc = &ink_malloc;
  pcre_free = &ink_free;

  if (is_debug_tag_set("url_rewrite"))
    Print();
}


UrlRewrite::~UrlRewrite()
{
  xfree(this->file_var);
  xfree(this->ts_name);
  xfree(this->http_default_redirect_url);

  DestroyStore(forward_mappings);
  DestroyStore(reverse_mappings);
  DestroyStore(permanent_redirects);
  DestroyStore(temporary_redirects);

  if (remap_pi_list) {
    remap_pi_list->delete_my_list();
    remap_pi_list = NULL;
  }
}

/** Sets the reverse proxy flag. */
void
UrlRewrite::SetReverseFlag(int flag)
{
  reverse_proxy = flag;
  if (is_debug_tag_set("url_rewrite"))
    Print();
}

/**
  Allocaites via new, and setups the default mapping to the PAC generator
  port which is used to serve the PAC (proxy autoconfig) file.

*/
url_mapping *
UrlRewrite::SetupPacMapping()
{
  const char *from_url = "http:///";
  const char *local_url = "http://127.0.0.1/";

  url_mapping *mapping;
  int pac_generator_port;

  mapping = new url_mapping;

  mapping->fromURL.create(NULL);
  mapping->fromURL.parse(from_url, strlen(from_url));

  mapping->_default_to_url.create(NULL);
  mapping->_default_to_url.parse(local_url, strlen(local_url));

  pac_generator_port = (default_to_pac_port < 0) ? mgmt_autoconf_port : default_to_pac_port;

  mapping->_default_to_url.port_set(pac_generator_port);

  return mapping;
}

/**
  Allocaites via new, and adds a mapping like this map /ink/rh
  http://{backdoor}/ink/rh

  These {backdoor} things are then rewritten in a request-hdr hook.  (In the
  future it might make sense to move the rewriting into HttpSM directly.)

*/
url_mapping *
UrlRewrite::SetupBackdoorMapping()
{
  const char from_url[] = "/ink/rh";
  const char to_url[] = "http://{backdoor}/ink/rh";

  url_mapping *mapping = new url_mapping;

  mapping->fromURL.create(NULL);
  mapping->fromURL.parse(from_url, sizeof(from_url) - 1);
  mapping->fromURL.scheme_set(URL_SCHEME_HTTP, URL_LEN_HTTP);

  mapping->_default_to_url.create(NULL);
  mapping->_default_to_url.parse(to_url, sizeof(to_url) - 1);

  return mapping;
}

/** Deallocated a hash table and all the url_mappings in it. */
void
UrlRewrite::_destroyTable(InkHashTable * h_table)
{
  InkHashTableEntry *ht_entry;
  InkHashTableIteratorState ht_iter;
  UrlMappingPathIndex *item;
  UrlMappingPathIndex::MappingList mappings;

  if (h_table != NULL) {        // Iterate over the hash tabel freeing up the all the url_mappings
    //   contained with in
    for (ht_entry = ink_hash_table_iterator_first(h_table, &ht_iter); ht_entry != NULL;) {
      item = (UrlMappingPathIndex *) ink_hash_table_entry_value(h_table, ht_entry);
      item->GetMappings(mappings);
      for (UrlMappingPathIndex::MappingList::iterator mapping_iter = mappings.begin();
           mapping_iter != mappings.end(); ++mapping_iter) {
        delete *mapping_iter;
      }
      mappings.clear();
      delete item;
      ht_entry = ink_hash_table_iterator_next(h_table, &ht_iter);
    }
    ink_hash_table_destroy(h_table);
  }
}

/** Debugging Method. */
void
UrlRewrite::Print()
{
  printf("URL Rewrite table with %d entries\n", num_rules_forward +
         num_rules_reverse + num_rules_redirect_temporary + num_rules_redirect_permanent);
  printf("  Reverse Proxy is %s\n", (reverse_proxy == 0) ? "Off" : "On");

  if (forward_mappings.hash_lookup != NULL) {
    printf("  Forward Mapping Table with %d entries\n", num_rules_forward);
    PrintTable(forward_mappings.hash_lookup);
  }
  if (reverse_mappings.hash_lookup != NULL) {
    printf("  Reverse Mapping Table with %d entries\n", num_rules_reverse);
    PrintTable(reverse_mappings.hash_lookup);
  }
  if (permanent_redirects.hash_lookup != NULL) {
    printf("  Permanent Redirect Mapping Table with %d entries\n", num_rules_redirect_permanent);
    PrintTable(permanent_redirects.hash_lookup);
  }
  if (temporary_redirects.hash_lookup != NULL) {
    printf("  Temporary Redirect Mapping Table with %d entries\n", num_rules_redirect_temporary);
    PrintTable(temporary_redirects.hash_lookup);
  }
  if (http_default_redirect_url != NULL) {
    printf("  Referer filter default redirect URL: \"%s\"\n", http_default_redirect_url);
  }
}

/** Debugging method. */
void
UrlRewrite::PrintTable(InkHashTable * h_table)
{
  InkHashTableEntry *ht_entry;
  InkHashTableIteratorState ht_iter;
  UrlMappingPathIndex *value;
  char from_url_buf[2048], to_url_buf[2048];
  UrlMappingPathIndex::MappingList mappings;
  url_mapping *um;

  for (ht_entry = ink_hash_table_iterator_first(h_table, &ht_iter); ht_entry != NULL;) {
    value = (UrlMappingPathIndex *) ink_hash_table_entry_value(h_table, ht_entry);
    value->GetMappings(mappings);
    for (UrlMappingPathIndex::MappingList::iterator mapping_iter = mappings.begin();
         mapping_iter != mappings.end(); ++mapping_iter) {
      um = *mapping_iter;
      um->fromURL.string_get_buf(from_url_buf, (int) sizeof(from_url_buf));
      um->_default_to_url.string_get_buf(to_url_buf, (int) sizeof(to_url_buf));
      printf("\t %s %s=> %s %s <%s> [plugins %s enabled; running with %u plugins]\n", from_url_buf,
             um->unique ? "(unique)" : "", to_url_buf,
             um->homePageRedirect ? "(R)" : "", um->tag ? um->tag : "",
             um->_plugin_count > 0 ? "are" : "not", um->_plugin_count);
    }
    mappings.clear();
    ht_entry = ink_hash_table_iterator_next(h_table, &ht_iter);
  }
}

/**
  If a remapping is found, returns a pointer to it otherwise NULL is
  returned.

*/
url_mapping *
UrlRewrite::_tableLookup(InkHashTable * h_table, URL * request_url,
                        int request_port, char *request_host, int request_host_len, char *tag)
{
  NOWARN_UNUSED(tag);
  UrlMappingPathIndex *ht_entry;
  url_mapping *um = NULL;
  int ht_result;

  ht_result = ink_hash_table_lookup(h_table, request_host, (void **) &ht_entry);

  if (likely(ht_result && ht_entry)) {
    // for empty host don't do a normal search, get a mapping arbitrarily
    um = ht_entry->Search(request_url, request_port, request_host_len ? true : false);
  }
  return um;
}


/** Perform fast ACL filtering. */
void
UrlRewrite::PerformACLFiltering(HttpTransact::State * s, url_mapping * map)
{
  if (unlikely(!s || s->acl_filtering_performed || !s->client_connection_enabled))
    return;

  s->acl_filtering_performed = true;    // small protection against reverse mapping

  if (map->filter) {
    int i, res, method;
    i = (method = s->hdr_info.client_request.method_get_wksidx()) - HTTP_WKSIDX_CONNECT;
    if (likely(i >= 0 && i < ACL_FILTER_MAX_METHODS)) {
      bool client_enabled_flag = true;
      unsigned long client_ip = ntohl(s->client_info.ip);
      for (acl_filter_rule * rp = map->filter; rp; rp = rp->next) {
        bool match = true;
        if (rp->method_valid) {
          if (rp->method_idx[i] != method)
            match = false;
        }
        if (match && rp->src_ip_valid) {
          match = false;
          for (int j = 0; j < rp->src_ip_cnt && !match; j++) {
            res = (rp->src_ip_array[j].start <= client_ip && client_ip <= rp->src_ip_array[j].end) ? 1 : 0;
            if (rp->src_ip_array[j].invert) {
              if (res != 1)
                match = true;
            } else {
              if (res == 1)
                match = true;
            }
          }
        }
        if (match && client_enabled_flag) {     //make sure that a previous filter did not DENY
          Debug("url_rewrite", "matched ACL filter rule, %s request", rp->allow_flag ? "allowing" : "denying");
          client_enabled_flag = rp->allow_flag ? true : false;
        } else {
          if (!client_enabled_flag) {
            Debug("url_rewrite", "Previous ACL filter rule denied request, continuing to deny it");
          } else {
            Debug("url_rewrite", "did NOT match ACL filter rule, %s request", rp->allow_flag ? "denying" : "allowing");
              client_enabled_flag = rp->allow_flag ? false : true;
          }
        }

      }                         /* end of for(rp = map->filter;rp;rp = rp->next) */
      s->client_connection_enabled = client_enabled_flag;
    }
  }
}

/**
  Returns the length of the URL.

  Will replace the terminator with a '/' if this is a full URL and
  there are no '/' in it after the the host.  This ensures that class
  URL parses the URL correctly.

*/
int
UrlRewrite::UrlWhack(char *toWhack, int *origLength)
{
  int length = strlen(toWhack);
  char *tmp;
  *origLength = length;

  // Check to see if this a full URL
  tmp = strstr(toWhack, "://");
  if (tmp != NULL) {
    if (strchr(tmp + 3, '/') == NULL) {
      toWhack[length] = '/';
      length++;
    }
  }
  return length;
}

inline bool
UrlRewrite::_addToStore(MappingsStore &store, url_mapping *new_mapping, RegexMapping &reg_map,
                        char *src_host, bool is_cur_mapping_regex, int &count)
{
  bool retval;
  if (is_cur_mapping_regex) {
    store.regex_list.push_back(reg_map);
    retval = true;
  } else {
    retval = TableInsert(store.hash_lookup, new_mapping, src_host);
  }
  if (retval) {
    ++count;
  }
  return retval;
}

/**
  Reads the configuration file and creates a new hash table.

  @return zero on success and non-zero on failure.

*/
int
UrlRewrite::BuildTable()
{
  BUILD_TABLE_INFO bti;
  char *file_buf, errBuf[1024], errStrBuf[1024];
  Tokenizer whiteTok(" \t");
  bool alarm_already = false;
  const char *errStr;

  // Vars to parse line in file
  char *tok_state, *cur_line, *cur_line_tmp;
  int rparse, cur_line_size, cln = 0;        // Our current line number

  // Vars to build the mapping
  const char *fromScheme, *toScheme;
  int fromSchemeLen, toSchemeLen;
  const char *fromHost, *toHost;
  int fromHostLen, toHostLen;
  char *map_from, *map_from_start;
  char *map_to, *map_to_start;
  const char *tmp;              // Appease the DEC compiler
  char *fromHost_lower = NULL;
  char *fromHost_lower_ptr = NULL;
  char fromHost_lower_buf[1024];
  url_mapping *new_mapping;
  mapping_type maptype;
  referer_info *ri;
  int origLength;
  int length;
  int tok_count;

  RegexMapping reg_map;
  bool is_cur_mapping_regex;
  const char *type_id_str;
  bool add_result;

  ink_assert(forward_mappings.empty());
  ink_assert(reverse_mappings.empty());
  ink_assert(permanent_redirects.empty());
  ink_assert(temporary_redirects.empty());
  ink_assert(num_rules_forward == 0);
  ink_assert(num_rules_reverse == 0);
  ink_assert(num_rules_redirect_permanent == 0);
  ink_assert(num_rules_redirect_temporary == 0);

  memset(&bti, 0, sizeof(bti));

  if ((file_buf = readIntoBuffer(config_file_path, modulePrefix, NULL)) == NULL) {
    ink_error("Can't load remapping configuration file - %s", config_file_path);
    return 1;
  }

  forward_mappings.hash_lookup = ink_hash_table_create(InkHashTableKeyType_String);
  reverse_mappings.hash_lookup = ink_hash_table_create(InkHashTableKeyType_String);
  permanent_redirects.hash_lookup = ink_hash_table_create(InkHashTableKeyType_String);
  temporary_redirects.hash_lookup = ink_hash_table_create(InkHashTableKeyType_String);

  bti.paramc = (bti.argc = 0);
  memset(bti.paramv, 0, sizeof(bti.paramv));
  memset(bti.argv, 0, sizeof(bti.argv));

  Debug("url_rewrite", "[BuildTable] UrlRewrite::BuildTable()");

  for (cur_line = tokLine(file_buf, &tok_state); cur_line != NULL;) {
    errStrBuf[0] = 0;
    clear_xstr_array(bti.paramv, sizeof(bti.paramv) / sizeof(char *));
    clear_xstr_array(bti.argv, sizeof(bti.argv) / sizeof(char *));
    bti.paramc = (bti.argc = 0);

    // Strip leading whitespace
    while (*cur_line && isascii(*cur_line) && isspace(*cur_line))
      cur_line++;

    if ((cur_line_size = strlen((char *) cur_line)) <= 0) {
      cur_line = tokLine(NULL, &tok_state);
      cln++;
      continue;
    }
    // Strip trailing whitespace
    cur_line_tmp = cur_line + cur_line_size - 1;
    while (cur_line_tmp != cur_line && isascii(*cur_line_tmp) && isspace(*cur_line_tmp)) {
      *cur_line_tmp = '\0';
      cur_line_tmp--;
    }

    if ((cur_line_size = strlen((char *) cur_line)) <= 0 || *cur_line == '#' || *cur_line == '\0') {
      cur_line = tokLine(NULL, &tok_state);
      cln++;
      continue;
    }

    Debug("url_rewrite", "[BuildTable] Parsing: \"%s\"", cur_line);

    tok_count = whiteTok.Initialize(cur_line, SHARE_TOKS);

    for (int j = 0; j < tok_count; j++) {
      if (((char *) whiteTok[j])[0] == '@') {
        if (((char *) whiteTok[j])[1])
          bti.argv[bti.argc++] = xstrdup(&(((char *) whiteTok[j])[1]));
      } else {
        bti.paramv[bti.paramc++] = xstrdup((char *) whiteTok[j]);
      }
    }

    // Initial verification for number of arguments
    if (bti.paramc<1 || (bti.paramc < 3 && bti.paramv[0][0] != '.') || bti.paramc> BUILD_TABLE_MAX_ARGS) {
      snprintf(errBuf, sizeof(errBuf), "%s Malformed line %d in file %s", modulePrefix, cln + 1, config_file_path);
      Debug("url_rewrite", "[BuildTable] %s", errBuf);
      SignalError(errBuf, alarm_already);
      cur_line = tokLine(NULL, &tok_state);
      cln++;
      continue;
    }
    // just check all major flags/optional arguments
    bti.remap_optflg = check_remap_option(bti.argv, bti.argc);


    // Check directive keywords (starting from '.')
    if (bti.paramv[0][0] == '.') {
      if ((errStr = parse_directive(&bti, errStrBuf, sizeof(errStrBuf))) != NULL) {
        snprintf(errBuf, sizeof(errBuf) - 1, "%s Error on line %d - %s", modulePrefix, cln + 1, errStr);
        Debug("url_rewrite", "[BuildTable] %s", errBuf);
        SignalError(errBuf, alarm_already);
      }
      cur_line = tokLine(NULL, &tok_state);
      cln++;
      continue;
    }

    is_cur_mapping_regex = (strncasecmp("regex_", bti.paramv[0], 6) == 0);
    type_id_str = is_cur_mapping_regex ? (bti.paramv[0] + 6) : bti.paramv[0];

    // Check to see whether is a reverse or forward mapping
    if (!strcasecmp("reverse_map", type_id_str)) {
      Debug("url_rewrite", "[BuildTable] - REVERSE_MAP");
      maptype = REVERSE_MAP;
    } else if (!strcasecmp("map", type_id_str)) {
      Debug("url_rewrite", "[BuildTable] - %s",
            ((bti.remap_optflg & REMAP_OPTFLG_MAP_WITH_REFERER) == 0) ? "FORWARD_MAP" : "FORWARD_MAP_REFERER");
      maptype = ((bti.remap_optflg & REMAP_OPTFLG_MAP_WITH_REFERER) == 0) ? FORWARD_MAP : FORWARD_MAP_REFERER;
    } else if (!strcasecmp("redirect", type_id_str)) {
      Debug("url_rewrite", "[BuildTable] - PERMANENT_REDIRECT");
      maptype = PERMANENT_REDIRECT;
    } else if (!strcasecmp("redirect_temporary", type_id_str)) {
      Debug("url_rewrite", "[BuildTable] - TEMPORARY_REDIRECT");
      maptype = TEMPORARY_REDIRECT;
    } else if (!strcasecmp("map_with_referer", type_id_str)) {
      Debug("url_rewrite", "[BuildTable] - FORWARD_MAP_REFERER");
      maptype = FORWARD_MAP_REFERER;
    } else {
      snprintf(errBuf, sizeof(errBuf) - 1, "%s Unknown mapping type at line %d", modulePrefix, cln + 1);
      Debug("url_rewrite", "[BuildTable] - %s", errBuf);
      SignalError(errBuf, alarm_already);
      cur_line = tokLine(NULL, &tok_state);
      cln++;
      continue;
    }

    new_mapping = NEW(new url_mapping(cln));  // use line # for rank for now

    // apply filter rules if we have to
    if ((errStr = process_filter_opt(new_mapping, &bti, errStrBuf, sizeof(errStrBuf))) != NULL) {
      goto MAP_ERROR;
    }

    new_mapping->map_id = 0;
    if ((bti.remap_optflg & REMAP_OPTFLG_MAP_ID) != 0) {
      int idx = 0;
      char *c;
      int ret = check_remap_option(bti.argv, bti.argc, REMAP_OPTFLG_MAP_ID, &idx);
      if (ret & REMAP_OPTFLG_MAP_ID) {
        c = strchr(bti.argv[idx], (int) '=');
        new_mapping->map_id = (unsigned int) atoi(++c);
      }
    }

    map_from = bti.paramv[1];
    length = UrlWhack(map_from, &origLength);

    // FIX --- what does this comment mean?
    //
    // URL::create modified map_from so keep a point to
    //   the beginning of the string
    if ((tmp = (map_from_start = map_from)) != NULL && length > 2 && tmp[length - 1] == '/' && tmp[length - 2] == '/') {
      new_mapping->unique = true;
      length -= 2;
    }

    new_mapping->fromURL.create(NULL);
    rparse = new_mapping->fromURL.parse_no_path_component_breakdown(tmp, length);

    map_from_start[origLength] = '\0';  // Unwhack

    if (rparse != PARSE_DONE) {
      errStr = "Malformed From URL";
      goto MAP_ERROR;
    }

    map_to = bti.paramv[2];
    length = UrlWhack(map_to, &origLength);
    map_to_start = map_to;
    tmp = map_to;

    new_mapping->_default_to_url.create(NULL);
    rparse = new_mapping->_default_to_url.parse_no_path_component_breakdown(tmp, length);
    map_to_start[origLength] = '\0';    // Unwhack

    if (rparse != PARSE_DONE) {
      errStr = "Malformed To URL";
      goto MAP_ERROR;
    }

    fromScheme = new_mapping->fromURL.scheme_get(&fromSchemeLen);
    // If the rule is "/" or just some other relative path
    //   we need to default the scheme to http
    if (fromScheme == NULL || fromSchemeLen == 0) {
      new_mapping->fromURL.scheme_set(URL_SCHEME_HTTP, URL_LEN_HTTP);
      fromScheme = new_mapping->fromURL.scheme_get(&fromSchemeLen);
      new_mapping->wildcard_from_scheme = true;
    }
    toScheme = new_mapping->_default_to_url.scheme_get(&toSchemeLen);

    // Include support for HTTPS scheme
    // includes support for FILE scheme
    if ((fromScheme != URL_SCHEME_HTTP && fromScheme != URL_SCHEME_HTTPS &&
         fromScheme != URL_SCHEME_FILE &&
         fromScheme != URL_SCHEME_RTSP && fromScheme != URL_SCHEME_TUNNEL &&
         fromScheme != URL_SCHEME_MMS && fromScheme != URL_SCHEME_MMSU &&
         fromScheme != URL_SCHEME_MMST) ||
        (toScheme != URL_SCHEME_HTTP && toScheme != URL_SCHEME_HTTPS &&
         toScheme != URL_SCHEME_RTSP &&
         toScheme != URL_SCHEME_TUNNEL && toScheme != URL_SCHEME_MMS &&
         toScheme != URL_SCHEME_MMSU && toScheme != URL_SCHEME_MMST)) {
      errStr = "Only http, https, rtsp, mms, and tunnel remappings are supported";
      goto MAP_ERROR;
    }
    // Check if a tag is specified.
    if (bti.paramv[3] != NULL) {
      if (maptype == FORWARD_MAP_REFERER) {
        new_mapping->filter_redirect_url = xstrdup(bti.paramv[3]);
        if (!strcasecmp(bti.paramv[3], "<default>") || !strcasecmp(bti.paramv[3], "default") ||
            !strcasecmp(bti.paramv[3], "<default_redirect_url>") || !strcasecmp(bti.paramv[3], "default_redirect_url"))
          new_mapping->default_redirect_url = true;
        new_mapping->redir_chunk_list = redirect_tag_str::parse_format_redirect_url(bti.paramv[3]);
        for (int j = bti.paramc; j > 4; j--) {
          if (bti.paramv[j - 1] != NULL) {
            char refinfo_error_buf[1024];
            bool refinfo_error = false;

            ri =
              NEW(new
                  referer_info((char *) bti.paramv[j - 1], &refinfo_error, refinfo_error_buf,
                               sizeof(refinfo_error_buf)));
            if (refinfo_error) {
              snprintf(errBuf, sizeof(errBuf), "%s Incorrect Referer regular expression \"%s\" at line %d - %s",
                           modulePrefix, bti.paramv[j - 1], cln + 1, refinfo_error_buf);
              SignalError(errBuf, alarm_already);
              delete ri;
              ri = 0;
            }

            if (ri && ri->negative) {
              if (ri->any) {
                new_mapping->optional_referer = true;   /* referer header is optional */
                delete ri;
                ri = 0;
              } else {
                new_mapping->negative_referer = true;   /* we have negative referer in list */
              }
            }
            if (ri) {
              ri->next = new_mapping->referer_list;
              new_mapping->referer_list = ri;
            }
          }
        }
      } else {
        new_mapping->tag = xstrdup(&(bti.paramv[3][0]));
      }
    }
    // Check to see the fromHost remapping is a relative one
    fromHost = new_mapping->fromURL.host_get(&fromHostLen);
    if (fromHost == NULL || fromHostLen <= 0) {
      if (maptype == FORWARD_MAP || maptype == FORWARD_MAP_REFERER) {
        if (*map_from_start != '/') {
          errStr = "Relative remappings must begin with a /";
          goto MAP_ERROR;
        } else {
          fromHost = "";
          fromHostLen = 0;
        }
      } else {
        errStr = "Remap source in reverse mappings requires a hostname";
        goto MAP_ERROR;
      }
    }

    toHost = new_mapping->_default_to_url.host_get(&toHostLen);
    if (toHost == NULL || toHostLen <= 0) {
      errStr = "The remap destinations require a hostname";
      goto MAP_ERROR;
    }
    // Get rid of trailing slashes since they interfere
    //  with our ability to send redirects

    // You might be tempted to remove these lines but the new
    // optimized header system will introduce problems.  You
    // might get two slashes occasionally instead of one because
    // the rest of the system assumes that trailing slashes have
    // been removed.


    if (unlikely(fromHostLen >= (int) sizeof(fromHost_lower_buf))) {
      if (unlikely((fromHost_lower = (fromHost_lower_ptr = (char *) xmalloc(fromHostLen + 1))) == NULL)) {
        fromHost_lower = &fromHost_lower_buf[0];
        fromHostLen = (int) (sizeof(fromHost_lower_buf) - 1);
      }
    } else {
      fromHost_lower = &fromHost_lower_buf[0];
    }
    // Canonicalize the hostname by making it lower case
    memcpy(fromHost_lower, fromHost, fromHostLen);
    fromHost_lower[fromHostLen] = 0;
    LowerCaseStr(fromHost_lower);

    // set the normalized string so nobody else has to normalize this
    new_mapping->fromURL.host_set(fromHost_lower, fromHostLen);

    if (is_cur_mapping_regex) {
      // it's ok to reuse reg_map as previous content (if any)
      // would be "reference-copied" into regex mapping lists
      if (!_processRegexMappingConfig(fromHost_lower, new_mapping, reg_map)) {
        errStr = "Could not process regex mapping config line";
        goto MAP_ERROR;
      }
      Debug("url_rewrite_regex", "Configured regex rule for host [%s]", fromHost_lower);
    }

    add_result = false;

    switch (maptype) {
    case FORWARD_MAP:
    case FORWARD_MAP_REFERER:
      if ((add_result = _addToStore(forward_mappings, new_mapping, reg_map, fromHost_lower,
                                    is_cur_mapping_regex, num_rules_forward)) == true) {
        // @todo: is this applicable to regex mapping too?
        SetHomePageRedirectFlag(new_mapping, new_mapping->_default_to_url);
      }
      break;
    case REVERSE_MAP:
      add_result = _addToStore(reverse_mappings, new_mapping, reg_map, fromHost_lower,
                               is_cur_mapping_regex, num_rules_reverse);
      new_mapping->homePageRedirect = false;
      break;
    case PERMANENT_REDIRECT:
      add_result = _addToStore(permanent_redirects, new_mapping, reg_map, fromHost_lower,
                               is_cur_mapping_regex, num_rules_redirect_permanent);
      break;
    case TEMPORARY_REDIRECT:
      add_result = _addToStore(temporary_redirects, new_mapping, reg_map, fromHost_lower,
                               is_cur_mapping_regex, num_rules_redirect_temporary);
      break;
    default:
      // 'default' required to avoid compiler warning; unsupported map
      // type would have been dealt with much before this
      break;
    };

    if (!add_result) {
      goto MAP_WARNING;
    }

    // If a TS receives a request on a port which is set to tunnel mode
    // (ie, blind forwarding) and a client connects directly to the TS,
    // then the TS will use its IPv4 address and remap rules given
    // to send the request to its proper destination.
    // See HttpTransact::HandleBlindTunnel().
    // Therefore, for a remap rule like "map tunnel://hostname..."
    // in remap.config, we also needs to convert hostname to its IPv4 addr
    // and gives a new remap rule with the IPv4 addr.
    if ((maptype == FORWARD_MAP || maptype == FORWARD_MAP_REFERER) &&
        fromScheme == URL_SCHEME_TUNNEL && (fromHost_lower[0]<'0' || fromHost_lower[0]> '9')) {
      ink_gethostbyname_r_data d;
      struct hostent *h;
      h = ink_gethostbyname_r(fromHost_lower, &d);
      // We only handle IPv4 addresses which are 4-byte long.
      if ((h != NULL) && (h->h_length == 4)) {
        char ipv4_name[128];
        url_mapping *u_mapping;
        for (int i = 0; h->h_addr_list[i] != NULL; i++) {
          ipv4_name[0] = '\0';
          int ip_len = snprintf(ipv4_name, sizeof(ipv4_name), "%hhu.%hhu.%hhu.%hhu",
                                (unsigned char) h->h_addr_list[i][0],
                                (unsigned char) h->h_addr_list[i][1],
                                (unsigned char) h->h_addr_list[i][2],
                                (unsigned char) h->h_addr_list[i][3]);
          if (ip_len > 0 && ip_len < (int) sizeof(ipv4_name)) {       // Create a new url mapping with the IPv4 address.
            u_mapping = NEW(new url_mapping);
            u_mapping->fromURL.create(NULL);
            u_mapping->fromURL.copy(&new_mapping->fromURL);
            u_mapping->fromURL.host_set(ipv4_name, strlen(ipv4_name));
            u_mapping->_default_to_url.create(NULL);
            u_mapping->_default_to_url.copy(&new_mapping->_default_to_url);
            if (bti.paramv[3] != NULL)
              u_mapping->tag = xstrdup(&(bti.paramv[3][0]));
            if (!TableInsert(forward_mappings.hash_lookup, u_mapping, ipv4_name)) {
              goto MAP_WARNING;
            }
            num_rules_forward++;
            SetHomePageRedirectFlag(u_mapping, u_mapping->_default_to_url);
          }
        }
      }
    }

    // For a remap rule like "map mms://proxy.com/ mms://origin_server/"
    // or "reverse_map mms://origin_server/ mms://proxy.com/",
    // we convert proxy.com to its IPv4 addr and gives a new remap rule
    // with the IPv4 addr.
    if ((maptype == FORWARD_MAP || maptype == FORWARD_MAP_REFERER) &&
        (fromScheme == URL_SCHEME_MMS ||
         (fromScheme == URL_SCHEME_HTTP && new_mapping->tag && strncmp(new_mapping->tag, "WMT", 3) == 0)) &&
        (fromHost_lower[0]<'0' || fromHost_lower[0]> '9')) {
      ink_gethostbyname_r_data d;
      struct hostent *h;
      h = ink_gethostbyname_r(fromHost_lower, &d);
      // We only handle IPv4 addresses which are 4-byte long.
      if ((h != NULL) && (h->h_length == 4)) {
        char ipv4_name[128];
        url_mapping *u_mapping;
        for (int i = 0; h->h_addr_list[i] != NULL; i++) {
          ipv4_name[0] = '\0';
          int ip_len;
          ip_len = snprintf(ipv4_name, sizeof(ipv4_name), "%hhu.%hhu.%hhu.%hhu",
                            (unsigned char) h->h_addr_list[i][0],
                            (unsigned char) h->h_addr_list[i][1],
                            (unsigned char) h->h_addr_list[i][2], (unsigned char) h->h_addr_list[i][3]);
          if (ip_len > 0 && ip_len < (int) sizeof(ipv4_name)) {
            // Create a new url mapping with the IPv4 address.
            u_mapping = NEW(new url_mapping);
            u_mapping->fromURL.create(NULL);
            u_mapping->fromURL.copy(&new_mapping->fromURL);
            u_mapping->fromURL.host_set(ipv4_name, strlen(ipv4_name));
            u_mapping->_default_to_url.create(NULL);
            u_mapping->_default_to_url.copy(&new_mapping->_default_to_url);
            if (bti.paramv[3] != NULL)
              u_mapping->tag = xstrdup(&(bti.paramv[3][0]));
            if (!TableInsert(forward_mappings.hash_lookup, u_mapping, ipv4_name)) {
              goto MAP_WARNING;
            }
            num_rules_forward++;
            SetHomePageRedirectFlag(u_mapping, u_mapping->_default_to_url);
          }
        }
      }
    }
    if (maptype == REVERSE_MAP && fromScheme == URL_SCHEME_MMS && (toHost[0]<'0' || toHost[0]> '9')) {
      ink_gethostbyname_r_data d;
      struct hostent *h;
      char *toHost_null_ended = xstrndup(toHost, toHostLen);

      h = ink_gethostbyname_r(toHost_null_ended, &d);
      // We only handle IPv4 addresses which are 4-byte long.
      if ((h != NULL) && (h->h_length == 4)) {
        char ipv4_name[128];
        url_mapping *u_mapping;
        for (int i = 0; h->h_addr_list[i] != NULL; i++) {
          ipv4_name[0] = '\0';
          int ip_len;
          ip_len = snprintf(ipv4_name, sizeof(ipv4_name), "%hhu.%hhu.%hhu.%hhu",
                            (unsigned char) h->h_addr_list[i][0],
                            (unsigned char) h->h_addr_list[i][1],
                            (unsigned char) h->h_addr_list[i][2], (unsigned char) h->h_addr_list[i][3]);
          if (ip_len > 0 && ip_len < (int) sizeof(ipv4_name)) {       // Create a new url mapping with the IPv4 address.
            u_mapping = NEW(new url_mapping);
            u_mapping->fromURL.create(NULL);
            u_mapping->fromURL.copy(&new_mapping->fromURL);
            u_mapping->_default_to_url.create(NULL);
            u_mapping->_default_to_url.copy(&new_mapping->_default_to_url);
            u_mapping->_default_to_url.host_set(ipv4_name, strlen(ipv4_name));
            if (bti.paramv[3] != NULL)
              u_mapping->tag = xstrdup(&(bti.paramv[3][0]));
            if (!TableInsert(reverse_mappings.hash_lookup, u_mapping, fromHost_lower)) {
              goto MAP_WARNING;
            }
            num_rules_reverse++;
            u_mapping->homePageRedirect = false;
          }
        }
      }
      xfree(toHost_null_ended);
    }

    // Check "remap" plugin options and load .so object
    if ((bti.remap_optflg & REMAP_OPTFLG_PLUGIN) != 0 && (maptype == FORWARD_MAP || maptype == FORWARD_MAP_REFERER)) {
      if ((check_remap_option(bti.argv, bti.argc, REMAP_OPTFLG_PLUGIN, &tok_count) & REMAP_OPTFLG_PLUGIN) != 0) {
        int plugin_found_at = 0;

        // this loads the first plugin
        if (load_remap_plugin(bti.argv, bti.argc, new_mapping, errStrBuf, sizeof(errStrBuf), 0, &plugin_found_at)) {
          Debug("remap_plugin", "Remap plugin load error - %s", errStrBuf[0] ? errStrBuf : "Unknown error");
          Debug("url_rewrite", "Remap plugin load error - %s", errStrBuf[0] ? errStrBuf : "Unknown error");
          ink_error(errStrBuf);
          errStr = errStrBuf;
          goto MAP_ERROR;
        }
        //this loads any subsequent plugins (if present)
        while (plugin_found_at) {
          int ret = load_remap_plugin(bti.argv, bti.argc, new_mapping, errStrBuf, sizeof(errStrBuf), plugin_found_at,
                                      &plugin_found_at);
          if (ret) {
            Debug("remap_plugin", "Remap plugin load error - %s", errStrBuf[0] ? errStrBuf : "Unknown error");
            Debug("url_rewrite", "Remap plugin load error - %s", errStrBuf[0] ? errStrBuf : "Unknown error");
            ink_error(errStrBuf);
            errStr = errStrBuf;
            goto MAP_ERROR;
          }
        }
      }
    }

    if (unlikely(fromHost_lower_ptr))
      fromHost_lower_ptr = (char *) xfree_null(fromHost_lower_ptr);

    cur_line = tokLine(NULL, &tok_state);
    cln++;
    continue;

  MAP_WARNING:
    Error("Could not add rule at line #%d; Continuing with remaining lines", cln + 1);
    if (new_mapping) {
      delete new_mapping;
      new_mapping = NULL;
    }
    continue;

  MAP_ERROR:
    snprintf(errBuf, sizeof(errBuf), "%s %s at line %d", modulePrefix, errStr, cln + 1);
    SignalError(errBuf, alarm_already);
    if (new_mapping) {
      delete new_mapping;
      new_mapping = NULL;
    }
    cur_line = tokLine(NULL, &tok_state);
    cln++;
    return 1;
  }                             /* end of while(cur_line != NULL) */

  clear_xstr_array(bti.paramv, sizeof(bti.paramv) / sizeof(char *));
  clear_xstr_array(bti.argv, sizeof(bti.argv) / sizeof(char *));
  bti.paramc = (bti.argc = 0);

  // Add the mapping for backdoor urls if enabled.
  // This needs to be before the default PAC mapping for ""
  // since this is more specific
  if (unlikely(backdoor_enabled)) {
    new_mapping = SetupBackdoorMapping();
    if (TableInsert(forward_mappings.hash_lookup, new_mapping, "")) {
      num_rules_forward++;
    } else {
      Error("Could not insert backdoor mapping into store");
      delete new_mapping;
    }
  }
  // Add the default mapping to the manager PAC file
  //  if we need it
  if (default_to_pac) {
    new_mapping = SetupPacMapping();
    if (TableInsert(forward_mappings.hash_lookup, new_mapping, "")) {
      num_rules_forward++;
    } else {
      Error("Could not insert pac mapping into store");
      delete new_mapping;
    }
  }
  // Destroy unused tables
  if (num_rules_forward == 0) {
    forward_mappings.hash_lookup = ink_hash_table_destroy(forward_mappings.hash_lookup);
  } else {
    if (ink_hash_table_isbound(forward_mappings.hash_lookup, "")) {
      nohost_rules = 1;
    }
  }

  if (num_rules_reverse == 0) {
    reverse_mappings.hash_lookup = ink_hash_table_destroy(reverse_mappings.hash_lookup);
  }

  if (num_rules_redirect_permanent == 0) {
    permanent_redirects.hash_lookup = ink_hash_table_destroy(permanent_redirects.hash_lookup);
  }

  if (num_rules_redirect_temporary == 0) {
    temporary_redirects.hash_lookup = ink_hash_table_destroy(temporary_redirects.hash_lookup);
  }

  xfree(file_buf);

  return 0;
}

/**
  Inserts arg mapping in h_table with key src_host chaining the mapping
  of existing entries bound to src_host if necessary.

*/
bool
UrlRewrite::TableInsert(InkHashTable * h_table, url_mapping * mapping, const char *src_host)
{
  char src_host_tmp_buf[1];
  UrlMappingPathIndex *ht_contents;

  if (!src_host) {
    src_host = &src_host_tmp_buf[0];
    src_host_tmp_buf[0] = 0;
  }
  // Insert the new_mapping into hash table
  if (ink_hash_table_lookup(h_table, src_host, (void**) &ht_contents)) {
    // There is already a path index for this host
    if (ht_contents == NULL) {
      // why should this happen?
      Error("Found entry cannot be null!");
      return false;
    }
  } else {
    ht_contents = new UrlMappingPathIndex();
    ink_hash_table_insert(h_table, src_host, ht_contents);
  }
  if (!ht_contents->Insert(mapping)) {
    Error("Could not insert new mapping");
    return false;
  }
  return true;
}

int
UrlRewrite::load_remap_plugin(char *argv[], int argc, url_mapping * mp, char *errbuf, int errbufsize, int jump_to_argc,
                              int *plugin_found_at)
{
  TSRemapInterface ri;
  struct stat stat_buf;
  remap_plugin_info *pi;
  char *c, *err, tmpbuf[2048], *parv[1024], default_path[PATH_MAX];
  char *new_argv[1024];
  int idx = 0, retcode = 0;
  int parc = 0;

  *plugin_found_at = 0;

  memset(parv, 0, sizeof(parv));
  memset(new_argv, 0, sizeof(new_argv));
  tmpbuf[0] = 0;

  if (jump_to_argc != 0) {
    argc -= jump_to_argc;
    int i = 0;
    while (argv[i + jump_to_argc]) {
      new_argv[i] = argv[i + jump_to_argc];
      i++;
    }
    argv = new_argv;
    if (!check_remap_option(argv, argc, REMAP_OPTFLG_PLUGIN, &idx)) {
      return -1;
    }
  } else {
    if (unlikely(!mp || (check_remap_option(argv, argc, REMAP_OPTFLG_PLUGIN, &idx) & REMAP_OPTFLG_PLUGIN) == 0)) {
      snprintf(errbuf, errbufsize, "Can't find remap plugin keyword or \"url_mapping\" is NULL");
      return -1;                /* incorrect input data - almost impossible case */
    }
  }

  if (unlikely((c = strchr(argv[idx], (int) '=')) == NULL || !(*(++c)))) {
    snprintf(errbuf, errbufsize, "Can't find remap plugin file name in \"@%s\"", argv[idx]);
    return -2;                  /* incorrect input data */
  }

  if (stat(c, &stat_buf) != 0) {
    const char *plugin_default_path = TSPluginDirGet();

    // Try with the plugin path instead
    if (strlen(c) + strlen(plugin_default_path) > (PATH_MAX - 1)) {
      Debug("remap_plugin", "way too large a path specified for remap plugin");
      return -3;
    }

    snprintf(default_path, PATH_MAX, "%s/%s", plugin_default_path, c);
    Debug("remap_plugin", "attempting to stat default plugin path: %s", default_path);

    if (stat(default_path, &stat_buf) == 0) {
      Debug("remap_plugin", "stat successful on %s using that", default_path);
      c = &default_path[0];
    } else {
      snprintf(errbuf, errbufsize, "Can't find remap plugin file \"%s\"", c);
      return -3;
    }
  }

  Debug("remap_plugin", "using path %s for plugin", c);

  if (!remap_pi_list || (pi = remap_pi_list->find_by_path(c)) == 0) {
    pi = NEW(new remap_plugin_info(c));
    if (!remap_pi_list) {
      remap_pi_list = pi;
    } else {
      remap_pi_list->add_to_list(pi);
    }
    Debug("remap_plugin", "New remap plugin info created for \"%s\"", c);

    if ((pi->dlh = dlopen(c, RTLD_NOW)) == NULL) {
#if defined(freebsd)
      err = (char *)dlerror();
#else
      err = dlerror();
#endif
      snprintf(errbuf, errbufsize, "Can't load plugin \"%s\" - %s", c, err ? err : "Unknown dlopen() error");
      return -4;
    }
    pi->fp_tsremap_init = (_tsremap_init *) dlsym(pi->dlh, TSREMAP_FUNCNAME_INIT);
    pi->fp_tsremap_done = (_tsremap_done *) dlsym(pi->dlh, TSREMAP_FUNCNAME_DONE);
    pi->fptsremap_new_instance = (_tsremap_new_instance *) dlsym(pi->dlh, TSREMAP_FUNCNAME_NEW_INSTANCE);
    pi->fp_tsremap_delete_instance = (_tsremap_delete_instance *) dlsym(pi->dlh, TSREMAP_FUNCNAME_DELETE_INSTANCE);
    pi->fp_tsremap_remap = (_tsremap_remap *) dlsym(pi->dlh, TSREMAP_FUNCNAME_REMAP);
    pi->fp_tsremap_os_response = (_tsremap_os_response *) dlsym(pi->dlh, TSREMAP_FUNCNAME_OS_RESPONSE);

    if (!pi->fp_tsremap_init) {
      snprintf(errbuf, errbufsize, "Can't find \"%s\" function in remap plugin \"%s\"", TSREMAP_FUNCNAME_INIT, c);
      retcode = -10;
    } else if (!pi->fptsremap_new_instance) {
      snprintf(errbuf, errbufsize, "Can't find \"%s\" function in remap plugin \"%s\"",
                   TSREMAP_FUNCNAME_NEW_INSTANCE, c);
      retcode = -11;
    } else if (!pi->fp_tsremap_remap) {
      snprintf(errbuf, errbufsize, "Can't find \"%s\" function in remap plugin \"%s\"", TSREMAP_FUNCNAME_REMAP, c);
      retcode = -12;
    }
    if (retcode) {
      if (errbuf && errbufsize > 0)
        Debug("remap_plugin", "%s", errbuf);
      dlclose(pi->dlh);
      pi->dlh = NULL;
      return retcode;
    }
    memset(&ri, 0, sizeof(ri));
    ri.size = sizeof(ri);
    ri.tsremap_version = TSREMAP_VERSION;
    ri.fp_tsremap_interface = NULL;     /* we don't need it now */

    if ((retcode = pi->fp_tsremap_init(&ri, tmpbuf, sizeof(tmpbuf) - 1)) != 0) {
      Error("Failed to initialize plugin %s (non-zero retval) ... bailing out", pi->path);
      exit(-1);                 //see my comment re: exit() about 60 lines down
    }
    Debug("remap_plugin", "Remap plugin \"%s\" - initialization completed", c);
  }

  if (!pi->dlh) {
    snprintf(errbuf, errbufsize, "Can't load plugin \"%s\"", c);
    return -5;
  }

  if ((err = mp->fromURL.string_get(NULL)) == NULL) {
    snprintf(errbuf, errbufsize, "Can't load fromURL from URL class");
    return -6;
  }
  parv[parc++] = xstrdup(err);
  xfree(err);

  if ((err = mp->_default_to_url.string_get(NULL)) == NULL) {
    snprintf(errbuf, errbufsize, "Can't load toURL from URL class");
    return -6;
  }
  parv[parc++] = xstrdup(err);
  xfree(err);

  bool plugin_encountered = false;
  // how many plugin parameters we have for this remapping
  for (idx = 0; idx < argc && parc < (int) ((sizeof(parv) / sizeof(char *)) - 1); idx++) {

    if (plugin_encountered && !strncasecmp("plugin=", argv[idx], 7) && argv[idx][7]) {
      *plugin_found_at = idx;
      break;                    //if there is another plugin, lets deal with that later
    }

    if (!strncasecmp("plugin=", argv[idx], 7)) {
      plugin_encountered = true;
    }

    if (!strncasecmp("pparam=", argv[idx], 7) && argv[idx][7]) {
      parv[parc++] = &(argv[idx][7]);
    }
  }

  Debug("url_rewrite", "Viewing all parameters for config line");
  for (int k = 0; k < argc; k++) {
    Debug("url_rewrite", "Argument %d: %s", k, argv[k]);
  }

  Debug("url_rewrite", "Viewing parsed plugin parameters for %s: [%d]", pi->path, *plugin_found_at);
  for (int k = 0; k < parc; k++) {
    Debug("url_rewrite", "Argument %d: %s", k, parv[k]);
  }

  ihandle *ih = mp->get_another_instance(pi);
  Debug("remap_plugin", "creating new plugin instance");
  retcode = pi->fptsremap_new_instance(parc, parv, ih, tmpbuf, sizeof(tmpbuf) - 1);
  Debug("remap_plugin", "done creating new plugin instance");

  xfree(parv[0]);               // fromURL
  xfree(parv[1]);               // toURL

  if (retcode != 0) {
    mp->delete_instance(pi);
    snprintf(errbuf, errbufsize, "Can't create new remap instance for plugin \"%s\" - %s", c,
                 tmpbuf[0] ? tmpbuf : "Unknown plugin error");
    Error("Failed to create new instance for plugin %s (non-zero retval)... bailing out", pi->path);
 	 	 /**
		 * fail here, otherwise we *will* fail later
		 * and that's some jacked backtrace inside CreateTableLookup [see bug 2316658]
		 * at least this one will be obvious
		 * We *really* don't want to continue when a plugin failed to init. We can't
		 * guarantee we are remapping what the user thought we were going to remap.
		 * using something nice like exit() would be more ideal, but this should be
		 * caught in development, anyway.
     **/
    exit(-1);
    return -6;
  }

  mp->add_plugin(pi);

  return 0;
}

/**  First looks up the hash table for "simple" mappings and then the
     regex mappings.  Only higher-ranked regex mappings are examined if
     a hash mapping is found; or else all regex mappings are examined

     Returns highest-ranked mapping on success, NULL on failure
*/
bool
UrlRewrite::_mappingLookup(MappingsStore &mappings, URL *request_url,
                           int request_port, const char *request_host, int request_host_len,
                           UrlMappingContainer &mapping_container, char *tag)
{
  char request_host_lower[TSREMAP_RRI_MAX_HOST_SIZE];

  if (!request_host || !request_url ||
      (request_host_len < 0) || (request_host_len >= TSREMAP_RRI_MAX_HOST_SIZE)) {
    Debug("url_rewrite", "Invalid arguments!");
    return false;
  }

  // lowercase
  for (int i = 0; i < request_host_len; ++i) {
    request_host_lower[i] = tolower(request_host[i]);
  }
  request_host_lower[request_host_len] = 0;

  bool retval = false;
  int rank_ceiling = -1;
  url_mapping *mapping = _tableLookup(mappings.hash_lookup, request_url, request_port, request_host_lower,
                                      request_host_len, tag);
  if (mapping != NULL) {
    rank_ceiling = mapping->getRank();
    Debug("url_rewrite", "Found 'simple' mapping with rank %d", rank_ceiling);
    mapping_container.set(mapping);
    retval = true;
  }
  if (_regexMappingLookup(mappings.regex_list, request_url, request_port, request_host_lower, request_host_len,
                          tag, rank_ceiling, mapping_container)) {
    Debug("url_rewrite", "Using regex mapping with rank %d", (mapping_container.getMapping())->getRank());
    retval = true;
  }
  return retval;
}

// does not null terminate return string
int
UrlRewrite::_expandSubstitutions(int *matches_info, const RegexMapping &reg_map,
                                 const char *matched_string,
                                 char *dest_buf, int dest_buf_size)
{
  int cur_buf_size = 0;
  int token_start = 0;
  int n_bytes_needed;
  int match_index;
  for (int i = 0; i < reg_map.n_substitutions; ++i) {
    // first copy preceding bytes
    n_bytes_needed = reg_map.substitution_markers[i] - token_start;
    if ((cur_buf_size + n_bytes_needed) > dest_buf_size) {
      goto lOverFlow;
    }
    memcpy(dest_buf + cur_buf_size, reg_map.to_url_host_template + token_start, n_bytes_needed);
    cur_buf_size += n_bytes_needed;

    // then copy the sub pattern match
    match_index = reg_map.substitution_ids[i] * 2;
    n_bytes_needed = matches_info[match_index + 1] - matches_info[match_index];
    if ((cur_buf_size + n_bytes_needed) > dest_buf_size) {
      goto lOverFlow;
    }
    memcpy(dest_buf + cur_buf_size, matched_string + matches_info[match_index], n_bytes_needed);
    cur_buf_size += n_bytes_needed;

    token_start = reg_map.substitution_markers[i] + 2; // skip the place holder
  }

  // copy last few bytes (if any)
  if (token_start < reg_map.to_url_host_template_len) {
    n_bytes_needed = reg_map.to_url_host_template_len - token_start;
    if ((cur_buf_size + n_bytes_needed) > dest_buf_size) {
      goto lOverFlow;
    }
    memcpy(dest_buf + cur_buf_size, reg_map.to_url_host_template + token_start, n_bytes_needed);
    cur_buf_size += n_bytes_needed;
  }
  Debug("url_rewrite_regex", "Expanded substitutions and returning string [%.*s] with length %d",
        cur_buf_size, dest_buf, cur_buf_size);
  return cur_buf_size;

 lOverFlow:
  Error("Overflow while expanding substitutions");
  return 0;
}

bool
UrlRewrite::_regexMappingLookup(RegexMappingList &regex_mappings, URL *request_url, int request_port,
                                const char *request_host, int request_host_len, char *tag, int rank_ceiling,
                                UrlMappingContainer &mapping_container)
{
  NOWARN_UNUSED(tag);
  bool retval = false;
  RegexMappingList::iterator list_iter;

  if (rank_ceiling == -1) { // we will now look at all regex mappings
    rank_ceiling = INT_MAX;
    Debug("url_rewrite_regex", "Going to match all regexes");
  }
  else {
    Debug("url_rewrite_regex", "Going to match regexes with rank <= %d", rank_ceiling);
  }

  int request_scheme_len, reg_map_scheme_len;
  const char *request_scheme = request_url->scheme_get(&request_scheme_len), *reg_map_scheme;

  int request_path_len, reg_map_path_len;
  const char *request_path = request_url->path_get(&request_path_len), *reg_map_path;

  for (list_iter = regex_mappings.begin(); list_iter != regex_mappings.end(); ++list_iter) {
    RegexMapping &reg_map = *list_iter; // handy pointer
    int reg_map_rank = reg_map.url_map->getRank();
    if (reg_map_rank > rank_ceiling) {
      break;
    }

    reg_map_scheme = reg_map.url_map->fromURL.scheme_get(&reg_map_scheme_len);
    if ((request_scheme_len != reg_map_scheme_len) ||
        strncmp(request_scheme, reg_map_scheme, request_scheme_len)) {
      Debug("url_rewrite_regex", "Skipping regex with rank %d as scheme does not match request scheme",
            reg_map_rank);
      continue;
    }

    if (reg_map.url_map->fromURL.port_get() != request_port) {
      Debug("url_rewrite_regex", "Skipping regex with rank %d as regex map port does not match request port. "
            "regex map port: %d, request port %d",
            reg_map_rank, reg_map.url_map->fromURL.port_get(), request_port);
      continue;
    }

    reg_map_path = reg_map.url_map->fromURL.path_get(&reg_map_path_len);
    if ((request_path_len < reg_map_path_len) ||
        strncmp(reg_map_path, request_path, reg_map_path_len)) { // use the shorter path length here
      Debug("url_rewrite_regex", "Skipping regex with rank %d as path does not cover request path",
            reg_map_rank);
      continue;
    }

    int matches_info[MAX_REGEX_SUBS * 3];
    int match_result = pcre_exec(reg_map.re, reg_map.re_extra, request_host, request_host_len,
                                 0, 0, matches_info, (sizeof(matches_info) / sizeof(int)));
    if (match_result > 0) {
      Debug("url_rewrite_regex", "Request URL host [%.*s] matched regex in mapping of rank %d "
            "with %d possible substitutions", request_host_len, request_host, reg_map_rank, match_result);

      mapping_container.set(reg_map.url_map);

      char buf[TSREMAP_RRI_MAX_REDIRECT_URL];
      int buf_len;

      // Expand substitutions in the host field from the stored template
      buf_len = _expandSubstitutions(matches_info, reg_map, request_host, buf, TSREMAP_RRI_MAX_REDIRECT_URL);
      URL *expanded_url = mapping_container.createNewToURL();
      expanded_url->copy(&((reg_map.url_map)->_default_to_url));
      expanded_url->host_set(buf, buf_len);

      Debug("url_rewrite_regex", "Expanded toURL to [%.*s]",
            expanded_url->length_get(), expanded_url->string_get_ref());
      retval = true;
      break;
    } else if (match_result == PCRE_ERROR_NOMATCH) {
      Debug("url_rewrite_regex", "Request URL host [%.*s] did NOT match regex in mapping of rank %d",
            request_host_len, request_host, reg_map_rank);
    } else {
      Error("pcre_exec() failed with error code %d", match_result);
      break;
    }
  }

  return retval;
}

void
UrlRewrite::_destroyList(RegexMappingList &mappings)
{
  RegexMappingList::iterator list_iter;

  for (list_iter = mappings.begin(); list_iter != mappings.end(); ++list_iter) {
    RegexMapping &reg_map = *list_iter; // handy reference
    delete reg_map.url_map;
    if (reg_map.re) {
      pcre_free(reg_map.re);
    }
    if (reg_map.re_extra) {
      pcre_free(reg_map.re_extra);
    }
    if (reg_map.to_url_host_template) {
      ink_free(reg_map.to_url_host_template);
    }
  }
  mappings.clear();
}

/** will process the regex mapping configuration and create objects in
    output argument reg_map. It assumes existing data in reg_map is
    inconsequential and will be perfunctorily null-ed;
*/
bool
UrlRewrite::_processRegexMappingConfig(const char *from_host_lower, url_mapping *new_mapping,
                                       RegexMapping &reg_map)
{
  const char *str;
  int str_index;
  const char *to_host;
  int to_host_len;
  int substitution_id;
  int substitution_count = 0;

  reg_map.re = NULL;
  reg_map.re_extra = NULL;
  reg_map.to_url_host_template = NULL;
  reg_map.to_url_host_template_len = 0;
  reg_map.n_substitutions = 0;

  reg_map.url_map = new_mapping;

  // using from_host_lower (and not new_mapping->fromURL.host_get())
  // as this one will be NULL-terminated (required by pcre_compile)
  reg_map.re = pcre_compile(from_host_lower, 0, &str, &str_index, NULL);
  if (reg_map.re == NULL) {
    Error("pcre_compile failed! Regex has error starting at %s", from_host_lower + str_index);
    goto lFail;
  }

  reg_map.re_extra = pcre_study(reg_map.re, 0, &str);
  if ((reg_map.re_extra == NULL) && (str != NULL)) {
    Error("pcre_study failed with message [%s]", str);
    goto lFail;
  }

  int n_captures;
  if (pcre_fullinfo(reg_map.re, reg_map.re_extra, PCRE_INFO_CAPTURECOUNT, &n_captures) != 0) {
    Error("pcre_fullinfo failed!");
    goto lFail;
  }
  if (n_captures >= MAX_REGEX_SUBS) { // off by one for $0 (implicit capture)
    Error("Regex has %d capturing subpatterns (including entire regex); Max allowed: %d",
          n_captures + 1, MAX_REGEX_SUBS);
    goto lFail;
  }

  to_host = new_mapping->_default_to_url.host_get(&to_host_len);
  for (int i = 0; i < (to_host_len - 1); ++i) {
    if (to_host[i] == '$') {
      if (substitution_count > MAX_REGEX_SUBS) {
        Error("Cannot have more than %d substitutions in mapping with host [%s]",
              MAX_REGEX_SUBS, from_host_lower);
        goto lFail;
      }
      substitution_id = to_host[i + 1] - '0';
      if ((substitution_id < 0) || (substitution_id > n_captures)) {
        Error("Substitution id [%c] has no corresponding capture pattern in regex [%s]",
              to_host[i + 1], from_host_lower);
        goto lFail;
      }
      reg_map.substitution_markers[reg_map.n_substitutions] = i;
      reg_map.substitution_ids[reg_map.n_substitutions] = substitution_id;
      ++reg_map.n_substitutions;
    }
  }

  // so the regex itself is stored in fromURL.host; string to match
  // will be in the request; string to use for substitutions will be
  // in this buffer
  str = new_mapping->_default_to_url.host_get(&str_index); // reusing str and str_index
  reg_map.to_url_host_template_len = str_index;
  reg_map.to_url_host_template = static_cast<char *>(ink_malloc(str_index));
  memcpy(reg_map.to_url_host_template, str, str_index);

  return true;

 lFail:
  if (reg_map.re) {
    pcre_free(reg_map.re);
    reg_map.re = NULL;
  }
  if (reg_map.re_extra) {
    pcre_free(reg_map.re_extra);
    reg_map.re_extra = NULL;
  }
  if (reg_map.to_url_host_template) {
    ink_free(reg_map.to_url_host_template);
    reg_map.to_url_host_template = NULL;
    reg_map.to_url_host_template_len = 0;
  }
  return false;
}