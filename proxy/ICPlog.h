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



/****************************************************************************

  ICPlog.h


****************************************************************************/

#ifndef _ICPlog_h_
#define _ICPlog_h_

#include "HTTP.h"

//
// Logging object which encapsulates ICP query info required
// by the new logging subsystem to produce squid access log
// data for ICP queries.
//
class ICPlog
{
public:
  inline ICPlog(ICPPeerReadCont::PeerReadData * s)
  {
    _s = s;
  }
   ~ICPlog()
  {
  }
  ink_hrtime GetElapsedTime();
  sockaddr const* GetClientIP();
  SquidLogCode GetAction();
  const char *GetCode();
  int GetSize();
  const char *GetMethod();
  const char *GetURI();
  const char *GetIdent();
  SquidHierarchyCode GetHierarchy();
  const char *GetFromHost();
  const char *GetContentType();

private:
  ICPPeerReadCont::PeerReadData * _s;
};

// End of ICPlog.h

#endif // _ICPlog_h_
