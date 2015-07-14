//--------------------------------------------------------------------------
// Copyright (C) 2014-2015 Cisco and/or its affiliates. All rights reserved.
// Copyright (C) 2003-2013 Sourcefire, Inc.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------

/**
**  @file       hi_main.c
**
**  @author     Daniel Roelker <droelker@sourcefire.com>
**
**  @brief      This file wraps the HttpInspect functionality for Snort
**              and starts the HttpInspect flow.
**
**
**  The file takes a Packet structure from the Snort IDS to start the
**  HttpInspect flow.  This also uses the Stream Interface Module which
**  is also Snort-centric.  Mainly, just a wrapper to HttpInspect
**  functionality, but a key part to starting the basic flow.
**
**  The main bulk of this file is taken up with user configuration and
**  parsing.  The reason this is so large is because HttpInspect takes
**  very detailed configuration parameters for each specified server.
**  Hopefully every web server that is out there can be emulated
**  with these configuration options.
**
**  The main functions of note are:
**    - HttpInspectSnortConf::this is the configuration portion
**    - HttpInspect::this is the actual inspection flow
**
**  NOTES:
**
**  - 2.11.03:  Initial Development.  DJR
**  - 2.4.05:   Added tab_uri_delimiter config option.  AJM.
*/
#include "hi_main.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <limits.h>
#include <ctype.h>

#include "detect.h"
#include "protocols/packet.h"
#include "event.h"
#include "snort_debug.h"
#include "util.h"
#include "stream/stream_api.h"
#include "sfsnprintfappend.h"
#include "hi_return_codes.h"
#include "hi_ui_config.h"
#include "hi_ui_iis_unicode_map.h"
#include "hi_si.h"
#include "hi_mi.h"
#include "hi_norm.h"
#include "detection_util.h"
#include "profiler.h"
#include "hi_cmd_lookup.h"
#include "loggers/unified2_common.h"
#include "file_api/file_api.h"
#include "sf_email_attach_decode.h"
#include "protocols/tcp.h"
#include "framework/data_bus.h"

const HiSearchToken hi_patterns[] =
{
    { "<SCRIPT",         7,  HI_JAVASCRIPT },
    { NULL,              0, 0 }
};

const HiSearchToken html_patterns[] =
{
    { "JAVASCRIPT",      10, HTML_JS },
    { "ECMASCRIPT",      10, HTML_EMA },
    { "VBSCRIPT",         8, HTML_VB },
    { NULL,               0, 0 }
};

SearchTool* hi_javascript_search_mpse = nullptr;
SearchTool* hi_htmltype_search_mpse = nullptr;

static uint32_t xtra_trueip_id;
static uint32_t xtra_uri_id;
static uint32_t xtra_hname_id;
static uint32_t xtra_gzip_id;
static uint32_t xtra_jsnorm_id;

HISearch hi_js_search[HI_LAST];
HISearch hi_html_search[HTML_LAST];

THREAD_LOCAL const HISearch* hi_current_search = NULL;
THREAD_LOCAL HISearchInfo hi_search_info;

THREAD_LOCAL HIStats hi_stats;

THREAD_LOCAL uint32_t http_mask;
THREAD_LOCAL HttpBuffer http_buffer[HTTP_BUFFER_MAX];
THREAD_LOCAL DataBuffer HttpDecodeBuf;

typedef enum
{
    CONFIG_MAX_SPACES = 0,
    CONFIG_MAX_JS_WS
} SpaceType;

unsigned HttpFlowData::flow_id = 0;

void HttpFlowData::init()
{
    flow_id = FlowData::get_flow_id();
}

HttpFlowData::HttpFlowData() : FlowData(flow_id)
{
    memset(&session, 0, sizeof(session));
    init_decode_utf_state(&session.utf_state);
}

HttpFlowData::~HttpFlowData()
{
    FreeHttpSessionData(&session);
}

HttpSessionData* SetNewHttpSessionData(Packet* p, void*)
{
    HttpFlowData* fd = new HttpFlowData;
    printf("HTTP Inspect got Session Data!\n");
    p->flow->set_application_data(fd);
    return &fd->session;
}

static HttpSessionData* get_session_data(Flow* flow)
{
    HttpFlowData* fd = (HttpFlowData*)flow->get_application_data(
                           HttpFlowData::flow_id);

    return fd ? &fd->session : NULL;
}

void HttpInspectRegisterXtraDataFuncs()
{
    xtra_trueip_id = stream.reg_xtra_data_cb(GetHttpTrueIP);
    xtra_uri_id = stream.reg_xtra_data_cb(GetHttpUriData);
    xtra_hname_id = stream.reg_xtra_data_cb(GetHttpHostnameData);
    xtra_gzip_id = stream.reg_xtra_data_cb(GetHttpGzipData);
    xtra_jsnorm_id = stream.reg_xtra_data_cb(GetHttpJSNormData);
}

static void PrintFileDecompOpt(HTTPINSPECT_CONF* ServerConf)
{
    LogMessage("      Decompress response files: %s %s %s\n",
               ((ServerConf->file_decomp_modes & FILE_SWF_ZLIB_BIT) != 0) ? "SWF-ZLIB" : "",
               ((ServerConf->file_decomp_modes & FILE_SWF_LZMA_BIT) != 0) ? "SWF-LZMA" : "",
               ((ServerConf->file_decomp_modes & FILE_PDF_DEFL_BIT) != 0) ? "PDF-DEFL" : "");
}

static int PrintConfOpt(HTTPINSPECT_CONF_OPT* ConfOpt, const char* Option)
{
    if (!ConfOpt || !Option)
    {
        return HI_INVALID_ARG;
    }

    if (ConfOpt->on)
        LogMessage("      %s: ON\n", Option);
    else
        LogMessage("      %s: OFF\n", Option);

    return 0;
}

int PrintServerConf(HTTPINSPECT_CONF* ServerConf)
{
    char buf[STD_BUF+1];
    int iCtr;
    int iChar = 0;
    PROFILES prof;

    if (!ServerConf)
    {
        return HI_INVALID_ARG;
    }

    prof = ServerConf->profile;
    LogMessage("      Server profile: %s\n",
               prof==HI_DEFAULT ? "Default" :
               prof==HI_APACHE ? "Apache" :
               prof==HI_IIS ? "IIS" :
               prof==HI_IIS4 ? "IIS4" : "IIS5");

    LogMessage("      Server Flow Depth: %d\n", ServerConf->server_flow_depth);
    LogMessage("      Client Flow Depth: %d\n", ServerConf->client_flow_depth);
    LogMessage("      Max Chunk Length: %d\n", ServerConf->chunk_length);
    if (ServerConf->small_chunk_length.size > 0)
        LogMessage("      Small Chunk Length Evasion: chunk size <= %u, threshold >= %u times\n",
                   ServerConf->small_chunk_length.size, ServerConf->small_chunk_length.num);
    LogMessage("      Max Header Field Length: %d\n", ServerConf->max_hdr_len);
    LogMessage("      Max Number Header Fields: %d\n", ServerConf->max_headers);
    LogMessage("      Max Number of WhiteSpaces allowed with header folding: %d\n",
               ServerConf->max_spaces);
    LogMessage("      Inspect Pipeline Requests: %s\n",
               ServerConf->no_pipeline ? "NO" : "YES");
    LogMessage("      URI Discovery Strict Mode: %s\n",
               ServerConf->non_strict ? "NO" : "YES");
    LogMessage("      Allow Proxy Usage: %s\n",
               ServerConf->allow_proxy ? "YES" : "NO");
    LogMessage("      Oversize Dir Length: %d\n",
               ServerConf->long_dir);
    LogMessage("      Only inspect URI: %s\n",
               ServerConf->uri_only ? "YES" : "NO");
    LogMessage("      Normalize HTTP Headers: %s\n",
               ServerConf->normalize_headers ? "YES" : "NO");
    LogMessage("      Inspect HTTP Cookies: %s\n",
               ServerConf->enable_cookie ? "YES" : "NO");
    LogMessage("      Inspect HTTP Responses: %s\n",
               ServerConf->inspect_response ? "YES" : "NO");
    LogMessage("      Unlimited decompression of gzip data from responses: %s\n",
               ServerConf->unlimited_decompress ? "YES" : "NO");
    LogMessage("      Normalize Javascripts in HTTP Responses: %s\n",
               ServerConf->normalize_javascript ? "YES" : "NO");
    if (ServerConf->normalize_javascript)
    {
        if (ServerConf->max_js_ws)
            LogMessage(
                "      Max Number of WhiteSpaces allowed with Javascript Obfuscation in HTTP responses: %d\n",
                ServerConf->max_js_ws);
    }
    LogMessage("      Normalize HTTP Cookies: %s\n",
               ServerConf->normalize_cookies ? "YES" : "NO");
    LogMessage("      Enable XFF and True Client IP: %s\n",
               ServerConf->enable_xff ? "YES"  :  "NO");
    LogMessage("      Extended ASCII code support in URI: %s\n",
               ServerConf->extended_ascii_uri ? "YES" : "NO");
    LogMessage("      Log HTTP URI data: %s\n",
               ServerConf->log_uri ? "YES"  :  "NO");
    LogMessage("      Log HTTP Hostname data: %s\n",
               ServerConf->log_hostname ? "YES"  :  "NO");
    LogMessage("      Extract Gzip from responses: %s\n",
               ServerConf->extract_gzip ? "YES" : "NO");
    PrintFileDecompOpt(ServerConf);

    PrintConfOpt(&ServerConf->ascii, "Ascii");
    PrintConfOpt(&ServerConf->double_decoding, "Double Decoding");
    PrintConfOpt(&ServerConf->u_encoding, "%U Encoding");
    PrintConfOpt(&ServerConf->bare_byte, "Bare Byte");
    PrintConfOpt(&ServerConf->utf_8, "UTF 8");
    PrintConfOpt(&ServerConf->iis_unicode, "IIS Unicode");
    PrintConfOpt(&ServerConf->multiple_slash, "Multiple Slash");
    PrintConfOpt(&ServerConf->iis_backslash, "IIS Backslash");
    PrintConfOpt(&ServerConf->directory, "Directory Traversal");
    PrintConfOpt(&ServerConf->webroot, "Web Root Traversal");
    PrintConfOpt(&ServerConf->apache_whitespace, "Apache WhiteSpace");
    PrintConfOpt(&ServerConf->iis_delimiter, "IIS Delimiter");

    if (ServerConf->iis_unicode_map_filename)
    {
        LogMessage("      IIS Unicode Map Filename: %s\n",
                   ServerConf->iis_unicode_map_filename);
        LogMessage("      IIS Unicode Map Codepage: %d\n",
                   ServerConf->iis_unicode_codepage);
    }
    else if (ServerConf->iis_unicode_map)
    {
        LogMessage("      IIS Unicode Map: "
                   "GLOBAL IIS UNICODE MAP CONFIG\n");
    }
    else
    {
        LogMessage("      IIS Unicode Map:  NOT CONFIGURED\n");
    }

    /*
    **  Print out the non-rfc chars
    */
    memset(buf, 0, STD_BUF+1);
    SnortSnprintf(buf, STD_BUF + 1, "      Non-RFC Compliant Characters: ");
    for (iCtr = 0; iCtr < 256; iCtr++)
    {
        if (ServerConf->non_rfc_chars[iCtr])
        {
            sfsnprintfappend(buf, STD_BUF, "0x%.2x ", (u_char)iCtr);
            iChar = 1;
        }
    }

    if (!iChar)
    {
        sfsnprintfappend(buf, STD_BUF, "NONE");
    }

    LogMessage("%s\n", buf);

    /*
    **  Print out the whitespace chars
    */
    iChar = 0;
    memset(buf, 0, STD_BUF+1);
    SnortSnprintf(buf, STD_BUF + 1, "      Whitespace Characters: ");
    for (iCtr = 0; iCtr < 256; iCtr++)
    {
        if (ServerConf->whitespace[iCtr])
        {
            sfsnprintfappend(buf, STD_BUF, "0x%.2x ", (u_char)iCtr);
            iChar = 1;
        }
    }

    if (!iChar)
    {
        sfsnprintfappend(buf, STD_BUF, "NONE");
    }

    LogMessage("%s\n", buf);

    return 0;
}

int PrintGlobalConf(HTTPINSPECT_GLOBAL_CONF* GlobalConf)
{
    LogMessage("HttpInspect Config:\n");
    LogMessage("    GLOBAL CONFIG\n");

    LogMessage("      Detect Proxy Usage:       %s\n",
               GlobalConf->proxy_alert ? "YES" : "NO");
    LogMessage("      IIS Unicode Map Filename: %s\n",
               GlobalConf->iis_unicode_map_filename);
    LogMessage("      IIS Unicode Map Codepage: %d\n",
               GlobalConf->iis_unicode_codepage);
    LogMessage("      Memcap used for logging URI and Hostname: %u\n",
               GlobalConf->memcap);
    LogMessage("      Max Gzip Memory: %d\n",
               GlobalConf->max_gzip_mem);
    LogMessage("      Max Gzip sessions: %d\n",
               GlobalConf->max_gzip_sessions);
    LogMessage("      Gzip Compress Depth: %d\n",
               GlobalConf->compr_depth);
    LogMessage("      Gzip Decompress Depth: %d\n",
               GlobalConf->decompr_depth);

    return 0;
}

static inline int SetSiInput(HI_SI_INPUT* SiInput, Packet* p)
{
    sfip_copy(SiInput->sip, p->ptrs.ip_api.get_src());
    sfip_copy(SiInput->dip, p->ptrs.ip_api.get_dst());
    SiInput->sport = p->ptrs.sp;
    SiInput->dport = p->ptrs.dp;

    /*
    **  We now set the packet direction
    */
    if (p->flow && stream.is_midstream(p->flow))
    {
        SiInput->pdir = HI_SI_NO_MODE;
    }
    else if (p->packet_flags & PKT_FROM_SERVER)
    {
        SiInput->pdir = HI_SI_SERVER_MODE;
    }
    else if (p->packet_flags & PKT_FROM_CLIENT)
    {
        SiInput->pdir = HI_SI_CLIENT_MODE;
    }
    else
    {
        SiInput->pdir = HI_SI_NO_MODE;
    }

    return HI_SUCCESS;
}

static inline void ApplyClientFlowDepth(Packet* p, int flow_depth)
{
    switch (flow_depth)
    {
    case -1:
        // Inspect none of the client if there is normalized/extracted
        // URI/Method/Header/Body data */
        SetDetectLimit(p, 0);
        break;

    case 0:
        // Inspect all of the client, even if there is normalized/extracted
        // URI/Method/Header/Body data */
        /* XXX: HUGE performance hit here */
        SetDetectLimit(p, p->dsize);
        break;

    default:
        // Limit inspection of the client, even if there is normalized/extracted
        // URI/Method/Header/Body data */
        /* XXX: Potential performance hit here */
        if (flow_depth < p->dsize)
        {
            SetDetectLimit(p, flow_depth);
        }
        else
        {
            SetDetectLimit(p, p->dsize);
        }
        break;
    }
}

static inline FilePosition getFilePoistion(Packet* p)
{
    FilePosition position = SNORT_FILE_POSITION_UNKNOWN;

    if (p->is_full_pdu())
        position = SNORT_FILE_FULL;
    else if (p->is_pdu_start())
        position = SNORT_FILE_START;
    else if (p->packet_flags & PKT_PDU_TAIL)
        position = SNORT_FILE_END;
    else if (file_api->get_file_processed_size(p->flow))
        position = SNORT_FILE_MIDDLE;

    return position;
}

// FIXIT-P extra data masks should only be updated as extra data changes state
// eg just once when captured; this function is called on every packet and
// repeatedly sets the flags on session
static inline void HttpLogFuncs(
    HttpSessionData* hsd, Packet* p, int iCallDetect)
{
    if (!hsd)
        return;

    /* for pipelined HTTP requests */
    if ( !iCallDetect )
        stream.clear_extra_data(p->flow, p, 0);

    if (hsd->true_ip)
    {
        if (!(p->packet_flags & PKT_STREAM_INSERT) && !(p->packet_flags & PKT_REBUILT_STREAM))
            SetExtraData(p, xtra_trueip_id);
        else
            stream.set_extra_data(p->flow, p, xtra_trueip_id);
    }

    if (hsd->log_flags & HTTP_LOG_URI)
    {
        stream.set_extra_data(p->flow, p, xtra_uri_id);
    }

    if (hsd->log_flags & HTTP_LOG_HOSTNAME)
    {
        stream.set_extra_data(p->flow, p, xtra_hname_id);
    }

    if (hsd->log_flags & HTTP_LOG_JSNORM_DATA)
    {
        SetExtraData(p, xtra_jsnorm_id);
    }
    if (hsd->log_flags & HTTP_LOG_GZIP_DATA)
    {
        SetExtraData(p, xtra_gzip_id);
    }
}

static inline void setFileName(Packet* p)
{
    uint8_t* buf = NULL;
    uint32_t len = 0;
    uint32_t type = 0;
    GetHttpUriData(p->flow, &buf, &len, &type);
    file_api->set_file_name (p->flow, buf, len);
}

/*
**  NAME
**    HttpInspectMain::
*/
/**
**  This function calls the HttpInspect function that processes an HTTP
**  session.
**
**  We need to instantiate a pointer for the HI_SESSION that HttpInspect
**  fills in.  Right now stateless processing fills in this session, which
**  we then normalize, and eventually detect.  We'll have to handle
**  separately the normalization events, etc.
**
**  This function is where we can see from the highest level what the
**  HttpInspect flow looks like.
**
**  @param GlobalConf pointer to the global configuration
**  @param p          pointer to the Packet structure
**
**  @return integer
**
**  @retval  0 function successful
**  @retval <0 fatal error
**  @retval >0 non-fatal error
*/
int HttpInspectMain(HTTPINSPECT_CONF* conf, Packet* p)
{
    HI_SESSION* session;
    HI_SI_INPUT SiInput;
    int iInspectMode = 0;
    int iRet;
    int iCallDetect = 1;
    HttpSessionData* hsd = NULL;

    PROFILE_VARS;

    hi_stats.total++;

    /*
    **  Set up the HI_SI_INPUT pointer.  This is what the session_inspection()
    **  routines use to determine client and server traffic.  Plus, this makes
    **  the HttpInspect library very independent from snort.
    */
    SetSiInput(&SiInput, p);

    /*
    **  HTTPINSPECT PACKET FLOW::
    **
    **  session Inspection Module::
    **    The session Inspection Module retrieves the appropriate server
    **    configuration for sessions, and takes care of the stateless
    **    vs. stateful processing in order to do this.  Once this module
    **    does it's magic, we're ready for the primetime.
    **
    **  HTTP Inspection Module::
    **    This isn't really a module in HttpInspect, but more of a helper
    **    function that sends the data to the appropriate inspection
    **    routine (client, server, anomalous server detection).
    **
    **  HTTP Normalization Module::
    **    This is where we normalize the data from the HTTP Inspection
    **    Module.  The Normalization module handles what type of normalization
    **    to do (client, server).
    **
    **  HTTP Detection Module::
    **    This isn't being used in the first iteration of HttpInspect, but
    **    all the HTTP detection components of signatures will be.
    **
    **  HTTP Event Output Module::
    **    The Event Ouput Module handles any events that have been logged
    **    in the inspection, normalization, or detection phases.
    */

    /*
    **  session Inspection Module::
    */
    iRet = hi_si_session_inspection(conf, &session, &SiInput, &iInspectMode, p);
    if (iRet)
        return iRet;

    /* If no mode then we just look for anomalous servers if configured
     * to do so and get out of here */
    if (iInspectMode == HI_SI_NO_MODE)
    {
        /* Let's look for rogue HTTP servers and stuff */
        if (conf->global->anomalous_servers && (p->dsize > 5))
        {
            iRet = hi_server_anomaly_detection(session, p->data, p->dsize);
            if (iRet)
                return iRet;
        }

        return 0;
    }

    hsd = get_session_data(p->flow);

    if ( (p->packet_flags & PKT_STREAM_INSERT) && !p->is_full_pdu() )
    {
        int flow_depth;

        if ( iInspectMode == HI_SI_CLIENT_MODE )
        {
            flow_depth = session->server_conf->client_flow_depth;
            ApplyClientFlowDepth(p, flow_depth);
        }
        else
        {
            ApplyFlowDepth(session->server_conf, p, hsd, 0, 0, GET_PKT_SEQ(p));
        }

        p->packet_flags |= PKT_HTTP_DECODE;

        if ( p->alt_dsize == 0 )
        {
            DisableDetect(p);
            return 0;
        }
        // see comments on call to snort_detect() below
        MODULE_PROFILE_START(hiDetectPerfStats);
        get_data_bus().publish(PACKET_EVENT, p);
#ifdef PERF_PROFILING
        hiDetectCalled = 1;
#endif
        MODULE_PROFILE_END(hiDetectPerfStats);
        return 0;
    }

    if (hsd == NULL)
        hsd = SetNewHttpSessionData(p, (void*)session);
    else
    {
        /* Gzip data should not be logged with all the packets of the session.*/
        hsd->log_flags &= ~HTTP_LOG_GZIP_DATA;
        hsd->log_flags &= ~HTTP_LOG_JSNORM_DATA;
    }

    /*
    **  HTTP Inspection Module::
    **
    **  This is where we do the client/server inspection and find the
    **  various HTTP protocol fields.  We then normalize these fields and
    **  call the detection engine.
    **
    **  The reason for the loop is for pipelined requests.  Doing pipelined
    **  requests in this way doesn't require any memory or tracking overhead.
    **  Instead, we just process each request linearly.
    */
    do
    {
        /*
        **  INIT:
        **  We set this equal to zero (again) because of the pipelining
        **  requests.  We don't want to bail before we get to setting the
        **  URI, so we make sure here that this can't happen.
        */
        SetHttpDecode(0);
        ClearHttpBuffers();

        iRet = hi_mi_mode_inspection(session, iInspectMode, p, hsd);
        if (iRet)
        {
            if (hsd)
            {
                if (hsd->mime_ssn)
                {
                    uint8_t* end = ( uint8_t*)(p->data) + p->dsize;
                    file_api->process_mime_data(p->flow, p->data, end, hsd->mime_ssn, 1,
                                                SNORT_FILE_POSITION_UNKNOWN);
                }
                else if (file_api->get_file_processed_size(p->flow) >0)
                {
                    file_api->file_process(p->flow, (uint8_t*)p->data, p->dsize,
                                           getFilePoistion(p), true, false);
                }
            }
            return iRet;
        }

        iRet = hi_normalization(session, iInspectMode, hsd);
        if (iRet)
        {
            return iRet;
        }

        HttpLogFuncs(hsd, p, iCallDetect);

        /*
        **  Let's setup the pointers for the detection engine, and
        **  then go for it.
        */
        if ( iInspectMode == HI_SI_CLIENT_MODE )
        {
            const HttpBuffer* hb;
            ClearHttpBuffers();  // FIXIT-P needed here and right above??

            if ( session->client.request.uri_norm )
            {
                SetHttpBuffer(
                    HTTP_BUFFER_URI,
                    session->client.request.uri_norm,
                    session->client.request.uri_norm_size,
                    session->client.request.uri_encode_type);

                SetHttpBuffer(
                    HTTP_BUFFER_RAW_URI,
                    session->client.request.uri,
                    session->client.request.uri_size);

                p->packet_flags |= PKT_HTTP_DECODE;

                get_data_bus().publish(
                    "http_uri", session->client.request.uri_norm,
                    session->client.request.uri_norm_size, p->flow);
            }
            else if ( session->client.request.uri )
            {
                SetHttpBuffer(
                    HTTP_BUFFER_URI,
                    session->client.request.uri,
                    session->client.request.uri_size,
                    session->client.request.uri_encode_type);

                SetHttpBuffer(
                    HTTP_BUFFER_RAW_URI,
                    session->client.request.uri,
                    session->client.request.uri_size);

                p->packet_flags |= PKT_HTTP_DECODE;

                get_data_bus().publish(
                    "http_raw_uri", session->client.request.uri,
                    session->client.request.uri_size, p->flow);
            }

            if ( session->client.request.header_norm ||
                    session->client.request.header_raw )
            {
                if ( session->client.request.header_norm )
                {
                    SetHttpBuffer(
                        HTTP_BUFFER_HEADER,
                        session->client.request.header_norm,
                        session->client.request.header_norm_size,
                        session->client.request.header_encode_type);

                    SetHttpBuffer(
                        HTTP_BUFFER_RAW_HEADER,
                        session->client.request.header_raw,
                        session->client.request.header_raw_size);

                    p->packet_flags |= PKT_HTTP_DECODE;
                }
                else
                {
                    SetHttpBuffer(
                        HTTP_BUFFER_HEADER,
                        session->client.request.header_raw,
                        session->client.request.header_raw_size,
                        session->client.request.header_encode_type);

                    SetHttpBuffer(
                        HTTP_BUFFER_RAW_HEADER,
                        session->client.request.header_raw,
                        session->client.request.header_raw_size);

                    p->packet_flags |= PKT_HTTP_DECODE;
                }
            }

            if (session->client.request.method & (HI_POST_METHOD | HI_GET_METHOD))
            {
                if (session->client.request.post_raw)
                {
                    uint8_t* start = (uint8_t*)(session->client.request.content_type);

                    if ( hsd && start )
                    {
                        /* mime parsing
                         * mime boundary should be processed before this
                         */
                        uint8_t* end;

                        if (!hsd->mime_ssn)
                        {
                            hsd->mime_ssn = (MimeState*)SnortAlloc(sizeof(MimeState));
                            if (!hsd->mime_ssn)
                                return 0;
                            hsd->mime_ssn->log_config = &(conf->global->mime_conf);
                            hsd->mime_ssn->decode_conf = &(conf->global->decode_conf);
                            /*Set log buffers per session*/
                            if (file_api->set_log_buffers(
                                        &(hsd->mime_ssn->log_state), hsd->mime_ssn->log_config) < 0)
                            {
                                return 0;
                            }
                        }

                        end = (uint8_t*)(session->client.request.post_raw +
                                         session->client.request.post_raw_size);
                        file_api->process_mime_data(p->flow, start, end, hsd->mime_ssn, 1,
                                                    SNORT_FILE_POSITION_UNKNOWN);
                    }
                    else
                    {
                        if (file_api->file_process(p->flow,
                                                   (uint8_t*)session->client.request.post_raw,
                                                   (uint16_t)session->client.request.post_raw_size,
                                                   getFilePoistion(p), true, false))
                        {
                            setFileName(p);
                        }
                    }

                    if (session->server_conf->post_depth > -1)
                    {
                        if (session->server_conf->post_depth &&
                                ((int)session->client.request.post_raw_size >
                                 session->server_conf->post_depth))
                        {
                            session->client.request.post_raw_size =
                                session->server_conf->post_depth;
                        }
                        SetHttpBuffer(
                            HTTP_BUFFER_CLIENT_BODY,
                            session->client.request.post_raw,
                            session->client.request.post_raw_size,
                            session->client.request.post_encode_type);

                        p->packet_flags |= PKT_HTTP_DECODE;
                    }
                }
            }
            else if (hsd)
            {
                if (hsd->mime_ssn)
                {
                    uint8_t* end = ( uint8_t*)(p->data) + p->dsize;
                    file_api->process_mime_data(p->flow, p->data, end, hsd->mime_ssn, 1,
                                                SNORT_FILE_POSITION_UNKNOWN);
                }
                else if (file_api->get_file_processed_size(p->flow) >0)
                {
                    file_api->file_process(p->flow, (uint8_t*)p->data, p->dsize,
                                           getFilePoistion(p),
                                           true, false);
                }
            }

            if ( session->client.request.method_raw )
            {
                SetHttpBuffer(
                    HTTP_BUFFER_METHOD,
                    session->client.request.method_raw,
                    session->client.request.method_size);

                p->packet_flags |= PKT_HTTP_DECODE;
            }

            if ( session->client.request.cookie_norm ||
                    session->client.request.cookie.cookie )
            {
                if ( session->client.request.cookie_norm )
                {
                    SetHttpBuffer(
                        HTTP_BUFFER_COOKIE,
                        session->client.request.cookie_norm,
                        session->client.request.cookie_norm_size,
                        session->client.request.cookie_encode_type);

                    SetHttpBuffer(
                        HTTP_BUFFER_RAW_COOKIE,
                        session->client.request.cookie.cookie,
                        session->client.request.cookie.cookie_end -
                        session->client.request.cookie.cookie);

                    p->packet_flags |= PKT_HTTP_DECODE;
                }
                else
                {
                    SetHttpBuffer(
                        HTTP_BUFFER_COOKIE,
                        session->client.request.cookie.cookie,
                        session->client.request.cookie.cookie_end -
                        session->client.request.cookie.cookie,
                        session->client.request.cookie_encode_type);

                    SetHttpBuffer(
                        HTTP_BUFFER_RAW_COOKIE,
                        session->client.request.cookie.cookie,
                        session->client.request.cookie.cookie_end -
                        session->client.request.cookie.cookie);

                    p->packet_flags |= PKT_HTTP_DECODE;
                }
            }
            else if ( !session->server_conf->enable_cookie &&
                      (hb = GetHttpBuffer(HTTP_BUFFER_HEADER)) )
            {
                SetHttpBuffer(
                    HTTP_BUFFER_COOKIE, hb->buf, hb->length, hb->encode_type);

                hb = GetHttpBuffer(HTTP_BUFFER_RAW_HEADER);
                assert(hb);

                SetHttpBuffer(HTTP_BUFFER_RAW_COOKIE, hb->buf, hb->length);

                p->packet_flags |= PKT_HTTP_DECODE;
            }

            if ( IsLimitedDetect(p) )
            {
                ApplyClientFlowDepth(p, session->server_conf->client_flow_depth);

                if ( !GetHttpBufferMask() && (p->alt_dsize == 0)  )
                {
                    DisableDetect(p);
                    return 0;
                }
            }
        }
        else   /* Server mode */
        {
            const HttpBuffer* hb;

            /*
            **  We check here to see whether this was a server response
            **  header or not.  If the header size is 0 then, we know that this
            **  is not the header and don't do any detection.
            */
            if ( !(session->server_conf->inspect_response) &&
                    IsLimitedDetect(p) && !p->alt_dsize )
            {
                DisableDetect(p);
                return 0;
            }
            ClearHttpBuffers();

            if ( session->server.response.header_norm ||
                    session->server.response.header_raw )
            {
                if ( session->server.response.header_norm )
                {
                    SetHttpBuffer(
                        HTTP_BUFFER_HEADER,
                        session->server.response.header_norm,
                        session->server.response.header_norm_size,
                        session->server.response.header_encode_type);

                    SetHttpBuffer(
                        HTTP_BUFFER_RAW_HEADER,
                        session->server.response.header_raw,
                        session->server.response.header_raw_size);
                }
                else
                {
                    SetHttpBuffer(
                        HTTP_BUFFER_HEADER,
                        session->server.response.header_raw,
                        session->server.response.header_raw_size);

                    SetHttpBuffer(
                        HTTP_BUFFER_RAW_HEADER,
                        session->server.response.header_raw,
                        session->server.response.header_raw_size);
                }
            }

            if ( session->server.response.cookie_norm ||
                    session->server.response.cookie.cookie )
            {
                if (session->server.response.cookie_norm )
                {
                    SetHttpBuffer(
                        HTTP_BUFFER_COOKIE,
                        session->server.response.cookie_norm,
                        session->server.response.cookie_norm_size,
                        session->server.response.cookie_encode_type);

                    SetHttpBuffer(
                        HTTP_BUFFER_RAW_COOKIE,
                        session->server.response.cookie.cookie,
                        session->server.response.cookie.cookie_end -
                        session->server.response.cookie.cookie);
                }
                else
                {
                    SetHttpBuffer(
                        HTTP_BUFFER_COOKIE,
                        session->server.response.cookie.cookie,
                        session->server.response.cookie.cookie_end -
                        session->server.response.cookie.cookie);

                    SetHttpBuffer(
                        HTTP_BUFFER_RAW_COOKIE,
                        session->server.response.cookie.cookie,
                        session->server.response.cookie.cookie_end -
                        session->server.response.cookie.cookie);
                }
            }
            else if ( !session->server_conf->enable_cookie &&
                      (hb = GetHttpBuffer(HTTP_BUFFER_HEADER)) )
            {
                SetHttpBuffer(
                    HTTP_BUFFER_COOKIE, hb->buf, hb->length, hb->encode_type);

                hb = GetHttpBuffer(HTTP_BUFFER_RAW_HEADER);
                assert(hb);

                SetHttpBuffer(HTTP_BUFFER_RAW_COOKIE, hb->buf, hb->length);
            }

            if (session->server.response.status_code)
            {
                SetHttpBuffer(
                    HTTP_BUFFER_STAT_CODE,
                    session->server.response.status_code,
                    session->server.response.status_code_size);
            }

            if (session->server.response.status_msg)
            {
                SetHttpBuffer(
                    HTTP_BUFFER_STAT_MSG,
                    session->server.response.status_msg,
                    session->server.response.status_msg_size);
            }

            if (session->server.response.body_size > 0)
            {
                int detect_data_size = (int)session->server.response.body_size;

                /*body_size is included in the data_extracted*/
                if ((session->server_conf->server_flow_depth > 0) &&
                        (hsd->resp_state.data_extracted  < (session->server_conf->server_flow_depth +
                                (int)session->server.response.body_size)))
                {
                    /*flow_depth is smaller than data_extracted, need to subtract*/
                    if (session->server_conf->server_flow_depth < hsd->resp_state.data_extracted)
                        detect_data_size -= hsd->resp_state.data_extracted -
                                            session->server_conf->server_flow_depth;
                }
                else if (session->server_conf->server_flow_depth)
                {
                    detect_data_size = 0;
                }

                /* Do we have a file decompression object? */
                if ( hsd->fd_state != 0 )
                {
                    fd_status_t Ret_Code;

                    uint16_t Data_Len;
                    const uint8_t* Data;

                    hsd->fd_state->Next_In = (uint8_t*)(Data = session->server.response.body);
                    hsd->fd_state->Avail_In = (Data_Len = (uint16_t)detect_data_size);

                    (void)File_Decomp_SetBuf(hsd->fd_state);

                    Ret_Code = File_Decomp(hsd->fd_state);

                    if ( Ret_Code == File_Decomp_DecompError )
                    {
                        session->server.response.body = Data;
                        session->server.response.body_size = Data_Len;

                        hi_set_event(GID_HTTP_SERVER, hsd->fd_state->Error_Event);
                        File_Decomp_StopFree(hsd->fd_state);
                        hsd->fd_state = NULL;
                    }
                    /* If we didn't find a Sig, then clear the File_Decomp state
                       and don't keep looking. */
                    else if ( Ret_Code == File_Decomp_NoSig )
                    {
                        File_Decomp_StopFree(hsd->fd_state);
                        hsd->fd_state = NULL;
                    }
                    else
                    {
                        session->server.response.body = hsd->fd_state->Buffer;
                        session->server.response.body_size = hsd->fd_state->Total_Out;
                    }

                    set_file_data((uint8_t*)session->server.response.body,
                                  (uint16_t)session->server.response.body_size);
                }
                else
                {
                    set_file_data((uint8_t*)session->server.response.body, detect_data_size);
                }

                if (p->has_paf_payload()
                        && file_api->file_process(p->flow,
                                                  (uint8_t*)session->server.response.body,
                                                  (uint16_t)session->server.response.body_size,
                                                  getFilePoistion(p), false, false))
                {
                    setFileName(p);
                }
            }

            if ( IsLimitedDetect(p) &&
                    !GetHttpBufferMask() && (p->alt_dsize == 0)  )
            {
                DisableDetect(p);
                return 0;
            }
        }

        /*
        **  If we get here we either had a client or server request/response.
        **  We do the detection here, because we're starting a new paradigm
        **  about protocol decoders.
        **
        **  Protocol decoders are now their own detection engine, since we are
        **  going to be moving protocol field detection from the generic
        **  detection engine into the protocol module.  This idea scales much
        **  better than having all these Packet struct field checks in the
        **  main detection engine for each protocol field.
        */
        MODULE_PROFILE_START(hiDetectPerfStats);
        snort_detect(p);
#ifdef PERF_PROFILING
        hiDetectCalled = 1;
#endif
        MODULE_PROFILE_END(hiDetectPerfStats);

        /*
        **  We set the global detection flag here so that if request pipelines
        **  fail, we don't do any detection.
        */
        iCallDetect = 0;
    }
    while (session->client.request.pipeline_req);

    if ( iCallDetect == 0 )
    {
        /* snort_detect called at least once from above pkt processing loop. */
        // FIXIT this throws off nfp rules like this:
        // alert tcp any any -> any any ( sid:1; msg:"1"; flags:S; )
        // (check shutdown counts)
        DisableInspection(p);
    }

    return 0;
}

int HttpInspectInitializeGlobalConfig(HTTPINSPECT_GLOBAL_CONF* config)
{
    int iRet;

    if ( !config )
        return -1;

    iRet = hi_ui_config_init_global_conf(config);
    if (iRet)
        return iRet;

    iRet = hi_client_init();
    if (iRet)
        return iRet;

    file_api->set_mime_decode_config_defauts(&(config->decode_conf));
    file_api->set_mime_log_config_defauts(&(config->mime_conf));

    return 0;
}

void FreeHttpSessionData(void* data)
{
    HttpSessionData* hsd = (HttpSessionData*)data;

    if (hsd->decomp_state != NULL)
    {
        inflateEnd(&(hsd->decomp_state->d_stream));
        free(hsd->decomp_state);
    }

    if (hsd->log_state != NULL)
        free(hsd->log_state);

    if (hsd->true_ip)
        sfip_free(hsd->true_ip);

    file_api->free_mime_session(hsd->mime_ssn);

    if ( hsd->fd_state != 0 )
    {
        File_Decomp_StopFree(hsd->fd_state);
        hsd->fd_state = NULL;                  // ...just for good measure
    }
}

int GetHttpTrueIP(Flow* flow, uint8_t** buf, uint32_t* len, uint32_t* type)
{
    HttpSessionData* hsd = get_session_data(flow);

    if (!hsd->true_ip)
        return 0;

    if (hsd->true_ip->family == AF_INET6)
    {
        *type = EVENT_INFO_XFF_IPV6;
        *len = sizeof(struct in6_addr); /*ipv6 address size in bytes*/
    }
    else
    {
        *type = EVENT_INFO_XFF_IPV4;
        *len = sizeof(struct in_addr); /*ipv4 address size in bytes*/
    }

    *buf = hsd->true_ip->ip8;
    printf("Http True IP:%s\n", *buf);
    return 1;
}

int IsGzipData(Flow* flow)
{
    HttpSessionData* hsd = NULL;

    if (flow == NULL)
        return -1;

    hsd = get_session_data(flow);

    if (hsd == NULL)
        return -1;

    if ((hsd->log_flags & HTTP_LOG_GZIP_DATA) && (g_file_data.len > 0 ))
        return 0;
    else
        return -1;
}

int GetHttpGzipData(Flow* flow, uint8_t** buf, uint32_t* len, uint32_t* type)
{
    if (!IsGzipData(flow))
    {
        *buf = g_file_data.data;
        printf("Gzip Data:%s\n",*buf);
        *len = g_file_data.len;
        *type = EVENT_INFO_GZIP_DATA;
        return 1;
    }

    return 0;
}

int IsJSNormData(Flow* flow)
{
    HttpSessionData* hsd = NULL;

    if (flow == NULL)
        return -1;

    hsd = get_session_data(flow);

    if (hsd == NULL)
        return -1;

    if ((hsd->log_flags & HTTP_LOG_JSNORM_DATA) && (g_file_data.len > 0 ))
        return 0;
    else
        return -1;
}

int GetHttpJSNormData(Flow* flow, uint8_t** buf, uint32_t* len, uint32_t* type)
{
    if (!IsJSNormData(flow))
    {
        *buf = g_file_data.data;
        printf("JS Norm Data:%s\n",*buf);
        *len = g_file_data.len;
        *type = EVENT_INFO_JSNORM_DATA;
        return 1;
    }

    return 0;
}

/*
 * All codes starting from below is used for ModSecurity CRS
 *
 * Fakhri Zulkifli
 *
 */

#include "urldecode.h"

/* Function: urlDecode */
char *urlDecode(const char *str) {
    int d = 0; /* whether or not the string is decoded */

    char *dStr = (char *) malloc(strlen(str) + 1);
    char eStr[] = "00"; /* for a hex code */

    strcpy(dStr, str);

    while(!d) {
        d = 1;
        int i; /* the counter for the string */

        for(i=0; i<strlen(dStr); ++i) {

            if(dStr[i] == '%') {
                if(dStr[i+1] == 0)
                    return dStr;

                if(isxdigit(dStr[i+1]) && isxdigit(dStr[i+2])) {

                    d = 0;

                    /* combine the next to numbers into one */
                    eStr[0] = dStr[i+1];
                    eStr[1] = dStr[i+2];

                    /* convert it to decimal */
                    long int x = strtol(eStr, NULL, 16);

                    /* remove the hex */
                    memmove(&dStr[i+1], &dStr[i+3], strlen(&dStr[i+3])+1);

                    dStr[i] = x;
                }
            }
        }
    }

    return dStr;
}

#include "slre.h"

#define MAX_BRANCHES 100
#define MAX_BRACKETS 100
#define FAIL_IF(condition, error_code) if (condition) return (error_code)

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(ar) (sizeof(ar) / sizeof((ar)[0]))
#endif

#ifdef SLRE_DEBUG
#define DBG(x) printf x
#else
#define DBG(x)
#endif

struct bracket_pair {
    const char *ptr;  /* Points to the first char after '(' in regex  */
    int len;          /* Length of the text between '(' and ')'       */
    int branches;     /* Index in the branches array for this pair    */
    int num_branches; /* Number of '|' in this bracket pair           */
};

struct branch {
    int bracket_index;    /* index for 'struct bracket_pair brackets' */
    /* array defined below                      */
    const char *schlong;  /* points to the '|' character in the regex */
};

struct regex_info {
    /*
     *    * Describes all bracket pairs in the regular expression.
     *       * First entry is always present, and grabs the whole regex.
     *          */
    struct bracket_pair brackets[MAX_BRACKETS];
    int num_brackets;

    /*
     *    * Describes alternations ('|' operators) in the regular expression.
     *       * Each branch falls into a specific branch pair.
     *          */
    struct branch branches[MAX_BRANCHES];
    int num_branches;

    /* Array of captures provided by the user */
    struct slre_cap *caps;
    int num_caps;

    /* E.g. SLRE_IGNORE_CASE. See enum below */
    int flags;
};

static int is_metacharacter(const unsigned char *s) {
    static const char *metacharacters = "^$().[]*+?|\\Ssdbfnrtv";
    return strchr(metacharacters, *s) != NULL;
}

static int op_len(const char *re) {
    return re[0] == '\\' && re[1] == 'x' ? 4 : re[0] == '\\' ? 2 : 1;
}

static int set_len(const char *re, int re_len) {
    int len = 0;

    while (len < re_len && re[len] != ']') {
        len += op_len(re + len);
    }

    return len <= re_len ? len + 1 : -1;
}

static int get_op_len(const char *re, int re_len) {
    return re[0] == '[' ? set_len(re + 1, re_len - 1) + 1 : op_len(re);
}

static int is_quantifier(const char *re) {
    return re[0] == '*' || re[0] == '+' || re[0] == '?';
}

static int toi(int x) {
    return isdigit(x) ? x - '0' : x - 'W';
}

static int hextoi(const unsigned char *s) {
    return (toi(tolower(s[0])) << 4) | toi(tolower(s[1]));
}

static int match_op(const unsigned char *re, const unsigned char *s,
                    struct regex_info *info) {
    int result = 0;
    switch (*re) {
    case '\\':
        /* Metacharacters */
        switch (re[1]) {
        case 'S':
            FAIL_IF(isspace(*s), SLRE_NO_MATCH);
            result++;
            break;
        case 's':
            FAIL_IF(!isspace(*s), SLRE_NO_MATCH);
            result++;
            break;
        case 'd':
            FAIL_IF(!isdigit(*s), SLRE_NO_MATCH);
            result++;
            break;
        case 'b':
            FAIL_IF(*s != '\b', SLRE_NO_MATCH);
            result++;
            break;
        case 'f':
            FAIL_IF(*s != '\f', SLRE_NO_MATCH);
            result++;
            break;
        case 'n':
            FAIL_IF(*s != '\n', SLRE_NO_MATCH);
            result++;
            break;
        case 'r':
            FAIL_IF(*s != '\r', SLRE_NO_MATCH);
            result++;
            break;
        case 't':
            FAIL_IF(*s != '\t', SLRE_NO_MATCH);
            result++;
            break;
        case 'v':
            FAIL_IF(*s != '\v', SLRE_NO_MATCH);
            result++;
            break;

        case 'x':
            /* Match byte, \xHH where HH is hexadecimal byte representaion */
            FAIL_IF(hextoi(re + 2) != *s, SLRE_NO_MATCH);
            result++;
            break;

        default:
            /* Valid metacharacter check is done in bar() */
            FAIL_IF(re[1] != s[0], SLRE_NO_MATCH);
            result++;
            break;
        }
        break;

    case '|':
        FAIL_IF(1, SLRE_INTERNAL_ERROR);
        break;
    case '$':
        FAIL_IF(1, SLRE_NO_MATCH);
        break;
    case '.':
        result++;
        break;

    default:
        if (info->flags & SLRE_IGNORE_CASE) {
            FAIL_IF(tolower(*re) != tolower(*s), SLRE_NO_MATCH);
        } else {
            FAIL_IF(*re != *s, SLRE_NO_MATCH);
        }
        result++;
        break;
    }

    return result;
}

static int match_set(const char *re, int re_len, const char *s,
                     struct regex_info *info) {
    int len = 0, result = -1, invert = re[0] == '^';

    if (invert) re++, re_len--;

    while (len <= re_len && re[len] != ']' && result <= 0) {
        /* Support character range */
        if (re[len] != '-' && re[len + 1] == '-' && re[len + 2] != ']' &&
                re[len + 2] != '\0') {
            result = info->flags &&  SLRE_IGNORE_CASE ?
                     tolower(*s) >= tolower(re[len]) && tolower(*s) <= tolower(re[len + 2]) :
                     *s >= re[len] && *s <= re[len + 2];
            len += 3;
        } else {
            result = match_op((unsigned char *) re + len, (unsigned char *) s, info);
            len += op_len(re + len);
        }
    }
    return (!invert && result > 0) || (invert && result <= 0) ? 1 : -1;
}

static int doh(const char *s, int s_len, struct regex_info *info, int bi);

static int bar(const char *re, int re_len, const char *s, int s_len,
               struct regex_info *info, int bi) {
    /* i is offset in re, j is offset in s, bi is brackets index */
    int i, j, n, step;

    for (i = j = 0; i < re_len && j <= s_len; i += step) {

        /* Handle quantifiers. Get the length of the chunk. */
        step = re[i] == '(' ? info->brackets[bi + 1].len + 2 :
               get_op_len(re + i, re_len - i);

        DBG(("%s [%.*s] [%.*s] re_len=%d step=%d i=%d j=%d\n", __func__,
             re_len - i, re + i, s_len - j, s + j, re_len, step, i, j));

        FAIL_IF(is_quantifier(&re[i]), SLRE_UNEXPECTED_QUANTIFIER);
        FAIL_IF(step <= 0, SLRE_INVALID_CHARACTER_SET);

        if (i + step < re_len && is_quantifier(re + i + step)) {
            DBG(("QUANTIFIER: [%.*s]%c [%.*s]\n", step, re + i,
                 re[i + step], s_len - j, s + j));
            if (re[i + step] == '?') {
                int result = bar(re + i, step, s + j, s_len - j, info, bi);
                j += result > 0 ? result : 0;
                i++;
            } else if (re[i + step] == '+' || re[i + step] == '*') {
                int j2 = j, nj = j, n1, n2 = -1, ni, non_greedy = 0;

                /* Points to the regexp code after the quantifier */
                ni = i + step + 1;
                if (ni < re_len && re[ni] == '?') {
                    non_greedy = 1;
                    ni++;
                }

                do {
                    if ((n1 = bar(re + i, step, s + j2, s_len - j2, info, bi)) > 0) {
                        j2 += n1;
                    }
                    if (re[i + step] == '+' && n1 < 0) break;

                    if (ni >= re_len) {
                        /* After quantifier, there is nothing */
                        nj = j2;
                    } else if ((n2 = bar(re + ni, re_len - ni, s + j2,
                                         s_len - j2, info, bi)) >= 0) {
                        /* Regex after quantifier matched */
                        nj = j2 + n2;
                    }
                    if (nj > j && non_greedy) break;
                } while (n1 > 0);

                /*
                 *          * Even if we found one or more pattern, this branch will be executed,
                 *                   * changing the next captures.
                 *                            */
                if (n1 < 0 && n2 < 0 && re[i + step] == '*' &&
                        (n2 = bar(re + ni, re_len - ni, s + j, s_len - j, info, bi)) > 0) {
                    nj = j + n2;
                }

                DBG(("STAR/PLUS END: %d %d %d %d %d\n", j, nj, re_len - ni, n1, n2));
                FAIL_IF(re[i + step] == '+' && nj == j, SLRE_NO_MATCH);

                /* If while loop body above was not executed for the * quantifier,  */
                /* make sure the rest of the regex matches                          */
                FAIL_IF(nj == j && ni < re_len && n2 < 0, SLRE_NO_MATCH);

                /* Returning here cause we've matched the rest of RE already */
                return nj;
            }
            continue;
        }

        if (re[i] == '[') {
            n = match_set(re + i + 1, re_len - (i + 2), s + j, info);
            DBG(("SET %.*s [%.*s] -> %d\n", step, re + i, s_len - j, s + j, n));
            FAIL_IF(n <= 0, SLRE_NO_MATCH);
            j += n;
        } else if (re[i] == '(') {
            n = SLRE_NO_MATCH;
            bi++;
            FAIL_IF(bi >= info->num_brackets, SLRE_INTERNAL_ERROR);
            DBG(("CAPTURING [%.*s] [%.*s] [%s]\n",
                 step, re + i, s_len - j, s + j, re + i + step));

            if (re_len - (i + step) <= 0) {
                /* Nothing follows brackets */
                n = doh(s + j, s_len - j, info, bi);
            } else {
                int j2;
                for (j2 = 0; j2 <= s_len - j; j2++) {
                    if ((n = doh(s + j, s_len - (j + j2), info, bi)) >= 0 &&
                            bar(re + i + step, re_len - (i + step),
                                s + j + n, s_len - (j + n), info, bi) >= 0) break;
                }
            }

            DBG(("CAPTURED [%.*s] [%.*s]:%d\n", step, re + i, s_len - j, s + j, n));
            FAIL_IF(n < 0, n);
            if (info->caps != NULL && n > 0) {
                info->caps[bi - 1].ptr = s + j;
                info->caps[bi - 1].len = n;
            }
            j += n;
        } else if (re[i] == '^') {
            FAIL_IF(j != 0, SLRE_NO_MATCH);
        } else if (re[i] == '$') {
            FAIL_IF(j != s_len, SLRE_NO_MATCH);
        } else {
            FAIL_IF(j >= s_len, SLRE_NO_MATCH);
            n = match_op((unsigned char *) (re + i), (unsigned char *) (s + j), info);
            FAIL_IF(n <= 0, n);
            j += n;
        }
    }

    return j;
}

/* Process branch points */
static int doh(const char *s, int s_len, struct regex_info *info, int bi) {
    const struct bracket_pair *b = &info->brackets[bi];
    int i = 0, len, result;
    const char *p;

    do {
        p = i == 0 ? b->ptr : info->branches[b->branches + i - 1].schlong + 1;
        len = b->num_branches == 0 ? b->len :
              i == b->num_branches ? (int) (b->ptr + b->len - p) :
              (int) (info->branches[b->branches + i].schlong - p);
        DBG(("%s %d %d [%.*s] [%.*s]\n", __func__, bi, i, len, p, s_len, s));
        result = bar(p, len, s, s_len, info, bi);
        DBG(("%s <- %d\n", __func__, result));
    } while (result <= 0 && i++ < b->num_branches);  /* At least 1 iteration */

    return result;
}

static int baz(const char *s, int s_len, struct regex_info *info) {
    int i, result = -1, is_anchored = info->brackets[0].ptr[0] == '^';

    for (i = 0; i <= s_len; i++) {
        result = doh(s + i, s_len - i, info, 0);
        if (result >= 0) {
            result += i;
            break;
        }
        if (is_anchored) break;
    }

    return result;
}

static void setup_branch_points(struct regex_info *info) {
    int i, j;
    struct branch tmp;

    /* First, sort branches. Must be stable, no qsort. Use bubble algo. */
    for (i = 0; i < info->num_branches; i++) {
        for (j = i + 1; j < info->num_branches; j++) {
            if (info->branches[i].bracket_index > info->branches[j].bracket_index) {
                tmp = info->branches[i];
                info->branches[i] = info->branches[j];
                info->branches[j] = tmp;
            }
        }
    }

    /*
     *    * For each bracket, set their branch points. This way, for every bracket
     *       * (i.e. every chunk of regex) we know all branch points before matching.
     *          */
    for (i = j = 0; i < info->num_brackets; i++) {
        info->brackets[i].num_branches = 0;
        info->brackets[i].branches = j;
        while (j < info->num_branches && info->branches[j].bracket_index == i) {
            info->brackets[i].num_branches++;
            j++;
        }
    }
}

static int foo(const char *re, int re_len, const char *s, int s_len,
               struct regex_info *info) {
    int i, step, depth = 0;

    /* First bracket captures everything */
    info->brackets[0].ptr = re;
    info->brackets[0].len = re_len;
    info->num_brackets = 1;

    /* Make a single pass over regex string, memorize brackets and branches */
    for (i = 0; i < re_len; i += step) {
        step = get_op_len(re + i, re_len - i);

        if (re[i] == '|') {
            FAIL_IF(info->num_branches >= (int) ARRAY_SIZE(info->branches),
                    SLRE_TOO_MANY_BRANCHES);
            info->branches[info->num_branches].bracket_index =
                info->brackets[info->num_brackets - 1].len == -1 ?
                info->num_brackets - 1 : depth;
            info->branches[info->num_branches].schlong = &re[i];
            info->num_branches++;
        } else if (re[i] == '\\') {
            FAIL_IF(i >= re_len - 1, SLRE_INVALID_METACHARACTER);
            if (re[i + 1] == 'x') {
                /* Hex digit specification must follow */
                FAIL_IF(re[i + 1] == 'x' && i >= re_len - 3,
                        SLRE_INVALID_METACHARACTER);
                FAIL_IF(re[i + 1] ==  'x' && !(isxdigit(re[i + 2]) &&
                                               isxdigit(re[i + 3])), SLRE_INVALID_METACHARACTER);
            } else {
                FAIL_IF(!is_metacharacter((unsigned char *) re + i + 1),
                        SLRE_INVALID_METACHARACTER);
            }
        } else if (re[i] == '(') {
            FAIL_IF(info->num_brackets >= (int) ARRAY_SIZE(info->brackets),
                    SLRE_TOO_MANY_BRACKETS);
            depth++;  /* Order is important here. Depth increments first. */
            info->brackets[info->num_brackets].ptr = re + i + 1;
            info->brackets[info->num_brackets].len = -1;
            info->num_brackets++;
            FAIL_IF(info->num_caps > 0 && info->num_brackets - 1 > info->num_caps,
                    SLRE_CAPS_ARRAY_TOO_SMALL);
        } else if (re[i] == ')') {
            int ind = info->brackets[info->num_brackets - 1].len == -1 ?
                      info->num_brackets - 1 : depth;
            info->brackets[ind].len = (int) (&re[i] - info->brackets[ind].ptr);
            DBG(("SETTING BRACKET %d [%.*s]\n",
                 ind, info->brackets[ind].len, info->brackets[ind].ptr));
            depth--;
            FAIL_IF(depth < 0, SLRE_UNBALANCED_BRACKETS);
            FAIL_IF(i > 0 && re[i - 1] == '(', SLRE_NO_MATCH);
        }
    }

    FAIL_IF(depth != 0, SLRE_UNBALANCED_BRACKETS);
    setup_branch_points(info);

    return baz(s, s_len, info);
}

int slre_match(const char *regexp, const char *s, int s_len,
               struct slre_cap *caps, int num_caps, int flags) {
    struct regex_info info;

    /* Initialize info structure */
    info.flags = flags;
    info.num_brackets = info.num_branches = 0;
    info.num_caps = num_caps;
    info.caps = caps;

    DBG(("========================> [%s] [%.*s]\n", regexp, s_len, s));
    return foo(regexp, (int) strlen(regexp), s, s_len, &info);
}

int GetHttpUriData(Flow* flow, uint8_t** buf, uint32_t* len, uint32_t* type)
{
    HttpSessionData* hsd = NULL;

    char str[BUFSIZ];

    if (flow == NULL)
        return 0;

    hsd = get_session_data(flow);

    if (hsd == NULL)
        return 0;

    if (hsd->log_state && hsd->log_state->uri_bytes > 0)
    {
        *buf = hsd->log_state->uri_extracted;
        //printf("Raw Http Uri Data:%s\n",*buf);
        memcpy(&str, *buf, 100 * sizeof(*buf));
        //printf("%s\n", str);
        char *x = urlDecode(str);
        //printf("%s\n", str);
        printf("Decoded Http Uri Data:%s\n", x);
        //const char *request = " GET /index.html?id=<script>alert(document.domain)</script> HTTP/1.0\r\n\r\n";
        const char *request = x;
        struct slre_cap caps[4];

        if (slre_match("(?i)(<script[^>]*>[\\s\\S]*?<\\/script[^>]*>|<script[^>]*>[\\s\\S]*?<\\/script[\\s\\S]]*[\\s\\S]|<script[^>]*>[\\s\\S]*?<\\/script[\\s]*[\\s]|<script[^>]*>[\\s\\S]*?<\\/script|<script[^>]*>[\\s\\S]*?)",
                       request, strlen(request), caps, 4, 0) > 0) {
            printf("Method: [%.*s], URI: [%.*s]\n",
                   caps[0].len, caps[0].ptr,
                   caps[1].len, caps[1].ptr);
        } else {
            printf("Error parsing [%s]\n", request);
        }
        free(x);
        *len = hsd->log_state->uri_bytes;
        printf("Http Uri Bytes:%i\n", *len);
        *type = EVENT_INFO_HTTP_URI;
        return 1;
    }

    return 0;
}
/* ModSecurity CRS codes ended here. */

int GetHttpHostnameData(Flow* flow, uint8_t** buf, uint32_t* len, uint32_t* type)
{
    HttpSessionData* hsd = NULL;

    if (flow == NULL)
        return 0;

    hsd = get_session_data(flow);

    if (hsd == NULL)
        return 0;

    if (hsd->log_state && hsd->log_state->hostname_bytes > 0)
    {
        *buf = hsd->log_state->hostname_extracted;
        printf("Http Hostname Data:%s\n",*buf);
        *len = hsd->log_state->hostname_bytes;
        *type = EVENT_INFO_HTTP_HOSTNAME;
        return 1;
    }

    return 0;
}

void HI_SearchInit(void)
{
    const HiSearchToken* tmp;
    hi_javascript_search_mpse = new SearchTool();
    if (hi_javascript_search_mpse == NULL)
    {
        FatalError("%s(%d) Could not allocate memory for HTTP <script> tag search.\n",
                   __FILE__, __LINE__);
    }
    for (tmp = &hi_patterns[0]; tmp->name != NULL; tmp++)
    {
        hi_js_search[tmp->search_id].name = tmp->name;
        hi_js_search[tmp->search_id].name_len = tmp->name_len;
        hi_javascript_search_mpse->add(tmp->name, tmp->name_len, tmp->search_id);
    }
    hi_javascript_search_mpse->prep();

    hi_htmltype_search_mpse = new SearchTool();
    if (hi_htmltype_search_mpse == NULL)
    {
        FatalError("%s(%d) Could not allocate memory for HTTP <script> type search.\n",
                   __FILE__, __LINE__);
    }
    for (tmp = &html_patterns[0]; tmp->name != NULL; tmp++)
    {
        hi_html_search[tmp->search_id].name = tmp->name;
        hi_html_search[tmp->search_id].name_len = tmp->name_len;
        hi_htmltype_search_mpse->add(tmp->name, tmp->name_len, tmp->search_id);
    }
    hi_htmltype_search_mpse->prep();
}

void HI_SearchFree(void)
{
    if (hi_javascript_search_mpse != NULL)
        delete hi_javascript_search_mpse;

    if (hi_htmltype_search_mpse != NULL)
        delete hi_htmltype_search_mpse;
}

int HI_SearchStrFound(void* id, void*, int index, void*, void*)
{
    int search_id = (int)(uintptr_t)id;

    hi_search_info.id = search_id;
    hi_search_info.index = index;
    hi_search_info.length = hi_current_search[search_id].name_len;

    /* Returning non-zero stops search, which is okay since we only look for one at a time */
    return 1;
}

