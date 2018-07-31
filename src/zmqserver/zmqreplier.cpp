// Copyright (c) 2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <boost/thread/thread.hpp>
#include "chainparams.h"
#include "chain.h"
#include "zmqreplier.h"
#include "zmqabstract.h"
#include "main.h"
#include "util.h"
#include "rpc/server.h"
#include "script/standard.h"
#include "base58.h"
#include "client-api/json.hpp"
#include "client-api/zmq.h"
#include "zmqserver.h"
#include "znode-sync.h"
#include "net.h"
#include "script/ismine.h"
#include "wallet/wallet.h"
#include "wallet/wallet.cpp"
#include "wallet/rpcwallet.cpp"

#include "client-api/server.h"
#include "client-api/protocol.h"

using path = boost::filesystem::path;
using json = nlohmann::json;
extern CWallet* pwalletMain;

//*********** threads waiting for responses ***********//
void* CZMQOpenReplier::Thread()
{
    LogPrintf("ZMQ: IN REQREP_ZMQ_open\n");
    while (KEEPALIVE) {
        /* Create an empty ØMQ message to hold the message part. */
        /* message assumed to contain an API command to be executed with data */
        rc = zmq_msg_init (&request);

        /* Block until a message is available to be received from socket */
        if(!Wait()){
            break;
        }

        APIJSONRequest jreq;
        string requestStr = ReadRequest();
        try {
            // Parse request
            UniValue valRequest;
            if (!valRequest.read(requestStr))
                throw JSONAPIError(API_PARSE_ERROR, "Parse error");

            jreq.parse(valRequest);

            UniValue result = tableAPI.execute(jreq, false);

            // Send reply
            response = JSONAPIReply(result, NullUniValue);
            if(!SendResponse()){
                break;
            }

        } catch (const UniValue& objError) {
            response = JSONAPIReply(NullUniValue, objError);
            if(!SendResponse()){
                break;
            }
        } catch (const std::exception& e) {
            response = JSONAPIReply(NullUniValue, JSONAPIError(API_PARSE_ERROR, e.what()));
            if(!SendResponse()){
                break;
            }
            // void *ret;
            // pthread_exit(ret);
            return NULL;
        }
    }
    // void *ret;
    // pthread_exit(ret);
    return NULL;
}

void* CZMQAuthReplier::Thread(){
    LogPrintf("ZMQ: IN REQREP_ZMQ_auth\n");
    while (KEEPALIVE) {
        /* Create an empty ØMQ message to hold the message part. */
        /* message assumed to contain an API command to be executed with data */
        rc = zmq_msg_init (&request);

        /* Block until a message is available to be received from socket */
        if(!Wait()){
            break;
        }

        APIJSONRequest jreq;
        string requestStr = ReadRequest();
        try {
            // Parse request
            UniValue valRequest;
            if (!valRequest.read(requestStr))
                throw JSONAPIError(API_PARSE_ERROR, "Parse error");

            jreq.parse(valRequest);

            UniValue result = tableAPI.execute(jreq, true);

            // Send reply
            response = JSONAPIReply(result, NullUniValue);
            if(!SendResponse()){
                break;
            }

        } catch (const UniValue& objError) {
            response = JSONAPIReply(NullUniValue, objError);
            if(!SendResponse()){
                break;
            }
        } catch (const std::exception& e) {
            response = JSONAPIReply(NullUniValue, JSONAPIError(API_PARSE_ERROR, e.what()));
            if(!SendResponse()){
                break;
            }
            // void *ret;
            // pthread_exit(ret);
            return NULL;
        }
    }

    // void *ret;
    // pthread_exit(ret);
    return NULL;
}

// 'Wait' thread. hangs waiting for REQ
bool CZMQAbstractReplier::Wait(){

    if(rc==-1) return false;
    /* Block until a message is available to be received from socket */
    LogPrintf("waiting to receive msg..\n");
    rc = zmq_recvmsg (psocket, &request, 0);
    if(rc==-1) return false;

    return true;
}

std::string CZMQAbstractReplier::ReadRequest(){
    char* requestChars = (char*) malloc (rc + 1);
    memcpy (requestChars, zmq_msg_data (&request), rc);
    zmq_msg_close(&request);
    requestChars[rc]=0;
    return std::string(requestChars);
}

bool CZMQAbstractReplier::SendResponse(){
    /* Send reply */
    zmq_msg_t reply;
    rc = zmq_msg_init_size (&reply, response.size());
    if(rc==-1) return false;  
    std::memcpy (zmq_msg_data (&reply), response.data(), response.size());
    LogPrintf("ZMQ: Sending reply..\n");
    /* Block until a message is available to be sent from socket */   
    rc = zmq_sendmsg (psocket, &reply, 0);    
    if(rc==-1) return false;

    LogPrintf("ZMQ: Reply sent.\n");
    zmq_msg_close(&reply);

    return true;
}

bool CZMQAbstractReplier::Socket(){
    LogPrintf("ZMQ: setting up type in Socket.\n");
    pcontext = zmq_ctx_new();

    LogPrintf("ZMQ: created pcontext\n");

    psocket = zmq_socket(pcontext,ZMQ_REP);
    if(!psocket){
        //TODO fail
        LogPrintf("ZMQ: Failed to create psocket\n");
        return false;
    }
    return true;
}

bool CZMQAuthReplier::Auth(){
    if(CZMQAbstract::DEV_AUTH){
        vector<string> keys = readCert(CZMQAbstract::Server);

        string server_secret_key = keys.at(1);

        LogPrintf("ZMQ: secret_server_key: %s\n", server_secret_key);

        const int curve_server_enable = 1;
        zmq_setsockopt(psocket, ZMQ_CURVE_SERVER, &curve_server_enable, sizeof(curve_server_enable));
        zmq_setsockopt(psocket, ZMQ_CURVE_SECRETKEY, server_secret_key.c_str(), 40);
    }

    return true;
}

bool CZMQAbstractReplier::Bind(){
    string tcp = "tcp://*:";

    LogPrintf("Port in bind: %s\n", port);

    int rc = zmq_bind(psocket, tcp.append(port).c_str());
    if (rc == -1)
    {
        LogPrintf("ZMQ: Unable to send ZMQ msg\n");
        return false;
    }
    LogPrintf("ZMQ: Bound socket\n");
    return true;
}

bool CZMQAbstractReplier::Initialize()
{
    LogPrintf("ZMQ: Initialzing REPlier\n");
    assert(!psocket);
    //TODO error handling
    Socket();
    Auth();
    Bind();
    worker = new boost::thread(boost::bind(&CZMQAbstractReplier::Thread, this));
    //worker->join();
    LogPrintf("created and ran thread\n");
    return true;
}

void CZMQAbstractReplier::Shutdown()
{
    assert(psocket);

    LogPrint(NULL, "Close socket at authority %s\n", authority);

    int linger = 0;
    zmq_setsockopt(psocket, ZMQ_LINGER, &linger, sizeof(linger));
    zmq_close(psocket);
    psocket = 0;

    if (pcontext)
    {
        zmq_ctx_destroy(pcontext);
        pcontext = 0;
    }

    KEEPALIVE = 0;
}