/** @file

  Class to execute one (or more) remap plugin(s).

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

#include "RemapPlugins.h"

ClassAllocator<RemapPlugins> pluginAllocator("RemapPluginsAlloc");

TSRemapStatus
RemapPlugins::run_plugin(remap_plugin_info* plugin)
{
  TSRemapStatus plugin_retcode;
  TSRemapRequestInfo rri;
  url_mapping *map = _s->url_map.getMapping();
  URL *map_from = &(map->fromURL);
  URL *map_to = _s->url_map.getToURL();

  // This is the equivalent of TSHttpTxnClientReqGet(), which every remap plugin would
  // have to call.
  rri.requestBufp = reinterpret_cast<TSMBuffer>(_request_header);
  rri.requestHdrp = reinterpret_cast<TSMLoc>(_request_header->m_http);

  // Read-only URL's (TSMLoc's to the SDK)
  rri.mapFromUrl = reinterpret_cast<TSMLoc>(map_from->m_url_impl);
  rri.mapToUrl = reinterpret_cast<TSMLoc>(map_to->m_url_impl);
  rri.requestUrl = reinterpret_cast<TSMLoc>(_request_url->m_url_impl);

  rri.redirect = 0;

  // These are made to reflect the "defaults" that will be used in
  // the case where the plugins don't modify them. It's semi-weird
  // that the "from" and "to" URLs changes when chaining happens, but
  // it is necessary to get predictable behavior.
#if 0
  if (_cur == 0) {
    rri.remap_from_host = map_from->host_get(&rri.remap_from_host_size);
    rri.remap_from_port = map_from->port_get();
    rri.remap_from_path = map_from->path_get(&rri.remap_from_path_size);
    rri.from_scheme = map_from->scheme_get(&rri.from_scheme_len);
  } else {
    rri.remap_from_host = _request_url->host_get(&rri.remap_from_host_size);
    rri.remap_from_port = _request_url->port_get();
    rri.remap_from_path = _request_url->path_get(&rri.remap_from_path_size);
    rri.from_scheme = _request_url->scheme_get(&rri.from_scheme_len);
  }
#endif

  void* ih = map->get_instance(_cur);

  // Prepare State for the future
  if (_s && _cur == 0) {
    _s->fp_tsremap_os_response = plugin->fp_tsremap_os_response;
    _s->remap_plugin_instance = ih;
  }

  plugin_retcode = plugin->fp_tsremap_do_remap(ih, _s ? reinterpret_cast<TSHttpTxn>(_s->state_machine) : NULL, &rri);
  // TODO: Deal with negative return codes here
  if (plugin_retcode < 0) {
    plugin_retcode = TSREMAP_NO_REMAP;
  }

  // First step after plugin remap must be "redirect url" check
  if ((TSREMAP_DID_REMAP == plugin_retcode || TSREMAP_DID_REMAP_STOP == plugin_retcode) && rri.redirect) {
    if (_s->remap_redirect != NULL) {
      ats_free(_s->remap_redirect);
    }
    _s->remap_redirect = _request_url->string_get(NULL);
  }

  return plugin_retcode;
}

/**
  This is the equivalent of the old DoRemap().

  @return 1 when you are done doing crap (otherwise, you get re-called
    with scheudle_imm and i hope you have something more to do), else
    0 if you have something more do do (this isnt strict and we check
    there actually *is* something to do).

*/
int
RemapPlugins::run_single_remap()
{
  // I should patent this
  Debug("url_rewrite", "Running single remap rule for the %d%s time", _cur, _cur == 1 ? "st" : _cur == 2 ? "nd" : _cur == 3 ? "rd" : "th");

  remap_plugin_info *plugin;
  TSRemapStatus plugin_retcode;
  url_mapping *map = _s->url_map.getMapping();

  if (_request_header) {
    plugin = map->get_plugin(_cur);    //get the nth plugin in our list of plugins
  }
  else {
    plugin = NULL;
  }

  if (plugin) {
    Debug("url_rewrite", "Remapping rule id: %d matched; running it now", map->getRank());
    plugin_retcode = run_plugin(plugin);
  } else if (_cur > 0) {
    _cur++;
    Debug("url_rewrite", "There wasn't a plugin available for us to run. Completing all remap processing immediately");
    return 1;
  }
  else {
    plugin_retcode = TSREMAP_NO_REMAP;
  }

  _cur++;
  if (_s->remap_redirect) {    //if redirect was set, we need to use that.
    return 1;
  }

  if (TSREMAP_NO_REMAP == plugin_retcode || TSREMAP_NO_REMAP_STOP == plugin_retcode) {
    if (_cur == 1) {  //first time
      Debug("url_rewrite", "plugin did not change host, port or path, copying from mapping rule");
      rewrite_table->doRemap(_s->url_map, _request_url, false);
    }
  }

  if (TSREMAP_NO_REMAP_STOP == plugin_retcode || TSREMAP_DID_REMAP_STOP == plugin_retcode) {
    Debug("url_rewrite", "breaking remap plugin chain since last plugin said we should stop");
    return 1;
  }

  if (_cur > MAX_REMAP_PLUGIN_CHAIN) {
    Error("Called run_single_remap more than 10 times. Stopping this remapping insanity now");
    Debug("url_rewrite", "Called run_single_remap more than 10 times. Stopping this remapping insanity now");
    return 1;
  }

  if (_cur >= map->plugin_count) {
    //normally, we would callback into this function but we dont have anything more to do!
    Debug("url_rewrite", "We completed all remap plugins for this rule");
    return 1;
  } else {
    Debug("url_rewrite", "Completed single remap. Attempting another via immediate callback");
    return 0;
  }
}

int
RemapPlugins::run_remap(int event, Event* e)
{
  Debug("url_rewrite", "Inside RemapPlugins::run_remap with cur = %d", _cur);

  ink_assert(action.continuation);
  ink_assert(action.continuation);

  int ret = 0;

  /* make sure we weren't cancelled */
  if (action.cancelled) {
    mutex.clear();
    pluginAllocator.free(this); //ugly
    return EVENT_DONE;
  }

  switch (event) {
  case EVENT_IMMEDIATE:
    Debug("url_rewrite", "handling immediate event inside RemapPlugins::run_remap");
    ret = run_single_remap();
    /**
     * If ret !=0 then we are done with this processor and we call back into the SM;
     * otherwise, we call this function again immediately (which really isn't immediate)
     * thru the eventProcessor, thus forcing another run of run_single_remap() which will
     * then operate on _request_url, etc performing additional remaps (mainly another plugin run)
     **/
    if (ret) {
      action.continuation->handleEvent(EVENT_REMAP_COMPLETE, NULL);
      mutex.clear();
      action.mutex.clear();
      mutex = NULL;
      action.mutex = NULL;
      //THREAD_FREE(this, pluginAllocator, t);
      pluginAllocator.free(this);       //ugly
      return EVENT_DONE;
    } else {
      e->schedule_imm(event);
      return EVENT_CONT;
    }

    break;
  default:
    ink_assert(!"unknown event type");
    break;
  };
  return EVENT_DONE;
}
