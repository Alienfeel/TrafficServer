#!/bin/env perl

#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

use warnings;
use strict;
use Carp;

require 5.006;



#
# Check if the line has an INK* to TS* change requirement
#
# tsapi const struct sockaddr_storage *INKHttpTxnClientSockAddrGet(TSHttpTxn txnp);
my %INK_EXCLUDES = (
  "INKSTAT_TYPE_INT64" => 1,
  "INKSTAT_TYPE_FLOAT" => 1,
  "INKStatTypes" => 1,
  "INKStat" => 1,
  "INKCoupledStat" => 1,
  "INKStatCreate" => 1,
  "INKStatIntAddTo" => 1,
  "INKStatFloatAddTo" => 1,
  "INKStatDecrement" => 1,
  "INKStatIncrement" => 1,
  "INKStatIntGet" => 1,
  "INKStatFloatGet" => 1,
  "INKStatIntSet" => 1,
  "INKStatFloatSet" => 1,
  "INKStatCoupledGlobalCategoryCreate" => 1,
  "INKStatCoupledLocalCopyCreate" => 1,
  "INKStatCoupledLocalCopyDestroy" => 1,
  "INKStatCoupledGlobalAdd" => 1,
  "INKStatCoupledLocalAdd" => 1,
  "INKStatsCoupledUpdate" => 1,
);

sub ink2ts {
  my $tokens = shift || return 0;
  my $line = shift || return;
  my $ret = 0;

  foreach my $tok (@{$tokens}) {
    next if $INK_EXCLUDES{$tok};
    if ($tok =~ /^INK/) {
      if ($tok eq "INK_ERROR_PTR") {
        print "\t--> INK_ERROR_PTR is not used as of v3.0, but should be replaced with TS_ERROR_PTR in v2.1.x\n";
      } else {
        my $new = $tok;

        $new =~ s/INK/TS/;
        print "\t--> $tok() has changed to $new()\n";
      }
      $ret = 1;
    }
  }

  return $ret;
}

my $W_TSRETURNCODE = "returns TSReturnCode, check for == TS_SUCCESS (and not 0|1)";
my $W_RETURN_DIRECT = "returns the value directly, it can not fail (don't check for TS_ERROR)";
my $W_TSPARSERESULT = "returns TSParseResult, do not check for e.g. TS_ERROR";
my $W_OUTPUT = "provides the return value as an output parameter (pass a non-NULL pointer)";
my $W_MLOC_OUTPUT = "provides the return TSMLoc as an output parameter (pass a non-NULL pointer)";
my $W_VOID_RETURN = "returns void, it can never fail";
my $W_VOID_PTR_RETURN = "returns a void pointer directly, it can never fail";
my $W_INT = "returns the integer directly, it should not be compared to TS_ERROR";
my $W_UNSIGNED_INT = "returns the unsigned integer directly, it should not be compared to TS_ERROR";
my $W_CHAR_NULL = "returns the char* pointer directly, it can be NULL";
my $W_CHAR_NOT_NULL = "returns the char* pointer directly, it can never be NULL";
my $W_TIME_T = "returns the time_t directly";
my $W_NOT_NULL_LEN = "the length parameter can no longer be a NULL pointer";
my $W_TSCACHEKEY = "returns a TSCacheKey directly";
my $W_NO_NULL_LENGTH = "1";
my $W_NO_ERROR_PTR = "2";


my %TWO_2_THREE = (
  "TSPluginRegister" => [$W_TSRETURNCODE],
  "TSUrlCreate" => [$W_TSRETURNCODE, $W_MLOC_OUTPUT],
  "TSUrlClone" => [$W_TSRETURNCODE, $W_MLOC_OUTPUT],
  "TSUrlPrint" => [$W_VOID_RETURN],
  "TSUrlParse" => [$W_TSPARSERESULT],
  "TSMimeParserClear" => [$W_VOID_RETURN],
  "TSMimeParserDestroy" => [$W_VOID_RETURN],
  "TSMimeHdrCreate" => [$W_TSRETURNCODE, $W_MLOC_OUTPUT],
  "TSMimeHdrClone" => [$W_TSRETURNCODE, $W_MLOC_OUTPUT],
  "TSMimeHdrPrint" => [$W_VOID_RETURN],
  "TSMimeHdrParse" => [$W_TSPARSERESULT],
  "TSMimeHdrFieldCreate" => [$W_TSRETURNCODE, $W_MLOC_OUTPUT],
  "TSMimeHdrFieldCreateNamed" => [$W_TSRETURNCODE, $W_MLOC_OUTPUT],
  "TSMimeHdrFieldClone" => [$W_TSRETURNCODE, $W_MLOC_OUTPUT],
  "TSMimeHdrFieldValueStringGet" => [$W_CHAR_NOT_NULL],
  "TSMimeHdrFieldValueIntGet" => [$W_INT],
  "TSMimeHdrFieldValueUintGet" => [$W_INT],
  "TSMimeHdrFieldValueDateGet" => [$W_TIME_T],
  "TSHttpParserClear" => [$W_VOID_RETURN],
  "TSHttpParserDestroy" => [$W_VOID_RETURN],
  "TSHttpHdrDestroy" => [$W_VOID_RETURN],
  "TSHttpHdrClone" => [$W_TSRETURNCODE, $W_MLOC_OUTPUT],
  "TSHttpHdrPrint" => [$W_VOID_RETURN],
  "TSHttpHdrParseReq" => [$W_TSPARSERESULT],
  "TSHttpHdrParseResp" => [$W_TSPARSERESULT],
  "TSHttpHdrUrlGet" => [$W_TSRETURNCODE, $W_MLOC_OUTPUT],
  "TSThreadDestroy" => [$W_VOID_RETURN],
  "TSMutexLock" => [$W_VOID_RETURN],
  "TSMutexLockTry" => [$W_TSRETURNCODE],
  "TSMutexUnlock" => [$W_VOID_RETURN],
  "TSCacheKeyCreate" => [$W_TSCACHEKEY],
  "TSMgmtIntGet" => [$W_TSRETURNCODE, $W_OUTPUT],
  "TSMgmtCounterGet" => [$W_TSRETURNCODE, $W_OUTPUT],
  "TSMgmtFloatGet" => [$W_TSRETURNCODE, $W_OUTPUT],
  "TSMgmtStringGet" => [$W_TSRETURNCODE, $W_OUTPUT],
  "TSContDestroy" => [$W_VOID_RETURN],
  "TSContDataSet" => [$W_VOID_RETURN],
  "TSContDataSet" => [$W_VOID_RETURN],
  "TSHttpHookAdd" => [$W_VOID_RETURN],
  "TSHttpSsnHookAdd" => [$W_VOID_RETURN],
  "TSHttpSsnReenable" => [$W_VOID_RETURN],
  "TSHttpSsnTransactionCount" => [$W_INT],
  "TSHttpTxnHookAdd" => [$W_VOID_RETURN],
  "TSHttpTxnClientReqGet" => [$W_TSRETURNCODE],
  "TSHttpTxnClientRespGet" => [$W_TSRETURNCODE],
  "TSHttpTxnServerReqGet" => [$W_TSRETURNCODE],
  "TSHttpTxnServerRespGet" => [$W_TSRETURNCODE],
  "TSHttpTxnCachedReqGet" => [$W_TSRETURNCODE],
  "TSHttpTxnCachedRespGet" => [$W_TSRETURNCODE],
  "TSFetchPageRespGet" => [$W_TSRETURNCODE],
  "TSHttpTxnTransformRespGet" => [$W_TSRETURNCODE],
  "TSHttpTxnClientFdGet" => [$W_TSRETURNCODE, $W_OUTPUT],
  "TSHttpTxnErrorBodySet" => [$W_VOID_RETURN],
  "TSHttpTxnParentProxySet" => [$W_VOID_RETURN],
  "TSHttpTxnUntransformedRespCache" => [$W_VOID_RETURN],
  "TSHttpTxnTransformedRespCache" => [$W_VOID_RETURN],
  "TSHttpTxnReenable" => [$W_VOID_RETURN],
  "TSHttpTxnArgSet" => [$W_VOID_RETURN],
  "TSHttpTxnArgGet" => [$W_VOID_PTR_RETURN],
  "TSHttpSsnArgSet" => [$W_VOID_RETURN],
  "TSHttpSsnArgGet" => [$W_VOID_PTR_RETURN],
  "TSHttpTxnSetHttpRetBody" => [$W_VOID_RETURN],
  "TSHttpTxnSetHttpRetStatus" => [$W_VOID_RETURN],
  "TSHttpTxnActiveTimeoutSet" => [$W_VOID_RETURN],
  "TSHttpTxnConnectTimeoutSet" => [$W_VOID_RETURN],
  "TSHttpTxnDNSTimeoutSet" => [$W_VOID_RETURN],
  "TSHttpTxnNoActivityTimeoutSet" => [$W_VOID_RETURN],
  "TSHttpTxnIntercept" => [$W_VOID_RETURN],
  "TSHttpTxnServerIntercept" => [$W_VOID_RETURN],
  "TSHttpConnect" => [$W_RETURN_DIRECT],
  "TSFetchUrl" => [$W_VOID_RETURN],
  "TSFetchPages" => [$W_VOID_RETURN],
  "TSHttpIsInternalRequest" => [$W_TSRETURNCODE],
  "TSHttpAltInfoQualitySet" => [$W_VOID_RETURN],
  "TSActionCancel" => [$W_VOID_RETURN],
  "TSVConnClose" => [$W_VOID_RETURN],
  "TSVConnAbort" => [$W_VOID_RETURN],
  "TSVConnShutdown" => [$W_VOID_RETURN],
  "TSVConnCacheObjectSizeGet" => [$W_RETURN_DIRECT],
  "TSNetVConnRemoteIPGet" => [$W_RETURN_DIRECT], # TODO: Change to struct addrinfo??
  "TSNetVConnRemotePortGet" => [$W_RETURN_DIRECT], # TODO: Change to struct addrinfo??
  "TSHostLookupResultIPGet" => [$W_RETURN_DIRECT], # TODO: Change to struct addrinfo??
  "TSVIOReenable" => [$W_VOID_RETURN],
  "TSVIONBytesSet" => [$W_VOID_RETURN],
  "TSVIONDoneSet" => [$W_VOID_RETURN],
  "TSIOBufferWaterMarkGet" => [$W_RETURN_DIRECT],
  "TSIOBufferWaterMarkSet" => [$W_VOID_RETURN],
  "TSIOBufferDestroy" => [$W_VOID_RETURN],
  "TSIOBufferProduce" => [$W_VOID_RETURN],
  "TSIOBufferReaderFree" => [$W_VOID_RETURN],
  "TSIOBufferReaderConsume" => [$W_VOID_RETURN],
  "TSStatIntIncrement" => [$W_VOID_RETURN],
  "TSStatIntDecrement" => [$W_VOID_RETURN],
  "TSStatIntGet" => [$W_RETURN_DIRECT],
  "TSStatIntSet" => [$W_VOID_RETURN],
  "TSStatFindName" => [$W_TSRETURNCODE, $W_OUTPUT],
  "TSTextLogObjectFlush" => [$W_VOID_RETURN],
  "TSTextLogObjectHeaderSet" => [$W_VOID_RETURN],
  "TSTextLogObjectRollingEnabledSet" => [$W_VOID_RETURN],
  "TSTextLogObjectRollingIntervalSecSet" => [$W_VOID_RETURN],
  "TSTextLogObjectRollingOffsetHrSet" => [$W_VOID_RETURN],
  "TSHttpTxnAborted" => [$W_TSRETURNCODE],
  "TSIOBufferAppend" => [$W_VOID_RETURN],
  "TSHttpTxnClientReqHdrBytesGet" => [$W_RETURN_DIRECT],
  "TSHttpTxnClientReqBodyBytesGet" => [$W_RETURN_DIRECT],
  "TSHttpTxnServerReqHdrBytesGet" => [$W_RETURN_DIRECT],
  "TSHttpTxnServerReqBodyBytesGet" => [$W_RETURN_DIRECT],
  "TSHttpTxnPushedRespHdrBytesGet" => [$W_RETURN_DIRECT],
  "TSHttpTxnPushedRespBodyBytesGet" => [$W_RETURN_DIRECT],
  "TSSkipRemappingSet" => [$W_VOID_RETURN],
  "TSRedirectUrlSet" => [$W_VOID_RETURN],
  "TSHttpCurrentClientConnectionsGet" => [$W_RETURN_DIRECT],
  "TSHttpCurrentActiveClientConnectionsGet" => [$W_RETURN_DIRECT],
  "TSHttpCurrentIdleClientConnectionsGet" => [$W_RETURN_DIRECT],
  "TSHttpCurrentCacheConnectionsGet" => [$W_RETURN_DIRECT],
  "TSHttpCurrentServerConnectionsGet" => [$W_RETURN_DIRECT],
  "TSUrlStringGet" => [$W_NO_NULL_LENGTH],
  "TSHttpTxnEffectiveUrlStringGet" => [$W_NO_NULL_LENGTH],
  "TSUrlUserGet" => [$W_NO_NULL_LENGTH],
  "TSUrlPasswordGet" => [$W_NO_NULL_LENGTH],
  "TSUrlHostGet" => [$W_NO_NULL_LENGTH],
  "TSUrlPathGet" => [$W_NO_NULL_LENGTH],
  "TSUrlHttpParamsGet" => [$W_NO_NULL_LENGTH],
  "TSUrlHttpQueryGet" => [$W_NO_NULL_LENGTH],
  "TSUrlHttpFragmentGet" => [$W_NO_NULL_LENGTH],
  "TSMimeHdrFieldNameGet" => [$W_NO_NULL_LENGTH],
  "TSHttpHdrMethodGet" => [$W_NO_NULL_LENGTH],
  "TSHttpHdrReasonGet" => [$W_NO_NULL_LENGTH],
  "TSCacheKeyGet" => [$W_NO_NULL_LENGTH],
  "TSCacheHeaderKeyGet" => [$W_NO_NULL_LENGTH],
  "TSFetchRespGet" => [$W_NO_NULL_LENGTH],
  "TSHttpTxnConfigStringGet" => [$W_NO_NULL_LENGTH],
  "TS_ERROR_PTR" => [$W_NO_ERROR_PTR]
);


#
# Warning messages related to SDK v2 to v3 migration
#
sub two2three {
  my $tokens = shift || return;
  my $line = shift || return;
  my $ret = 0;

  return 0 if $line =~ /TSDebug/;
  foreach my $tok (@{$tokens}) {
    my $warns = $TWO_2_THREE{$tok};
    next unless $warns;

    foreach my $w (@{$warns}) {
      if ($w eq $W_NO_NULL_LENGTH) {
        print "--> $tok() <--\n";
        print "    + The length output parameter can not be NULL\n" if $line =~ /NULL/;
      } elsif ($w eq $W_NO_ERROR_PTR) {
        print "--> TS_ERROR_PTR <--\n";
        print "    + no APIs can return TS_ERROR_PTR, you should not compare it\n";
      } else {
        print "--> $tok() <--\n    + $w\n";
      }
    }
    $ret = 1;
  }

  return $ret;
}


#
# Main processor of the source code
#
sub process {
  my $file = shift || return;
  my $line = 1;

  if (!open(FILE, "<$file")) {
    carp "Can't open file $file";
    return;
  }

  while (<FILE>) {
    my @tokens = split(/[^a-zA-Z0-9_]/);

    if (ink2ts(\@tokens, $_) || two2three(\@tokens, $_)) {
      print "$file:$line:$_\n";
    }

    ++$line;
  }

  close(FILE);
}


#
# Loop over all files provided
#
foreach my $file (@ARGV) {
  process($file);
}