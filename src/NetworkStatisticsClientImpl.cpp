//
//  NetworkStatisticsClient.cpp
//  netstat-socket
//
//  Created by Alex Malone on 12/6/17.
//  Copyright © 2017 Ziften. All rights reserved.
//

#include "NTStatKernelStructHandler.hpp"

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>

#include <sys/utsname.h>
#include <sys/sys_domain.h>
#include <sys/kern_control.h>

#include <string.h> // memcmp
#include <string>
#include <map>
using namespace std;

// pthread macros

#include <pthread.h>
#define MUTEX_T pthread_mutex_t
#define MUINIT(pMu) pthread_mutex_init ( (pMu), 0)
#define MULOCK(pMu) pthread_mutex_lock ( pMu)
#define MUUNLOCK(pMu) pthread_mutex_unlock ( pMu)

// references to the factory functions to allocate struct handlers for kernel versions

NTStatKernelStructHandler* NewNTStatKernel2422();
NTStatKernelStructHandler* NewNTStatKernel2782();
NTStatKernelStructHandler* NewNTStatKernel3789();
NTStatKernelStructHandler* NewNTStatKernel3248();
NTStatKernelStructHandler* NewNTStatKernel4570();

// minimum ntstat.h definitions needed here

#define      NET_STAT_CONTROL_NAME   "com.apple.network.statistics"

enum
{
  // generic response messages
  NSTAT_MSG_TYPE_SUCCESS                  = 0
  ,NSTAT_MSG_TYPE_ERROR                   = 1
  
  // Requests
  ,NSTAT_MSG_TYPE_ADD_SRC                 = 1001
  ,NSTAT_MSG_TYPE_ADD_ALL_SRCS            = 1002
  ,NSTAT_MSG_TYPE_REM_SRC                 = 1003
  ,NSTAT_MSG_TYPE_QUERY_SRC               = 1004
  ,NSTAT_MSG_TYPE_GET_SRC_DESC            = 1005
  
  // Responses/Notfications
  ,NSTAT_MSG_TYPE_SRC_ADDED               = 10001
  ,NSTAT_MSG_TYPE_SRC_REMOVED             = 10002
  ,NSTAT_MSG_TYPE_SRC_DESC                = 10003
  ,NSTAT_MSG_TYPE_SRC_COUNTS              = 10004
};


typedef struct nstat_msg_error
{
  nstat_msg_hdr   hdr;
  u_int32_t               error;  // errno error
  u_int8_t        reserved[4];
} nstat_msg_error;

// tracking of messages
/*
struct MsgContext
{
  uint16_t seqnum;
  uint16_t msgtype;
  uint16_t loc;
  uint16_t pad;

  uint64_t to_i() { return *(uint64_t*)this; }
};

struct QMsg
{
  MsgContext       context;
  vector<uint8_t>  msg;

  QMsg(uint32_t msgtype_, uint64_t ctx_, uint16_t seqnum_, vector<uint8_t> v) : seqnum(seqnum_), contextid(ctx_ & 0x0FFFFFFFFUL), msgtype(msgtype_) { }
};
*/
// Make these true to add more debug logging

static bool _logDbg = false;
static bool _logTrace = false;
static bool _logErrors = false;

const int BUFSIZE = 2048;
string msg_name(uint32_t msg_type);
unsigned int getXnuVersion();

/*
 * Wrapper around NTStatStream so we can track srcRef
 */
struct NetstatSource
{
  NetstatSource(uint64_t srcRef, uint32_t providerId) : _srcRef(srcRef), _providerId(providerId), _haveDesc(false), obj() {}

  uint64_t _srcRef;
  uint32_t _providerId;
  bool     _haveDesc;
  NTStatStream obj;
};

/*
 * Implementation of NetworkStatisticsClient
 */
class NetworkStatisticsClientImpl : public NetworkStatisticsClient
{
public:
  NetworkStatisticsClientImpl(NetworkStatisticsListener* listener): _listener(listener), _map(), _keepRunning(false), _fd(0), _udpAdded(false), _withCounts(false), _gotCounts(false) //, _seqnum(0), _qmsgMap()
  {
    MUINIT(&_mapMutex);
  }

  //------------------------------------------------------------------------
  // returns true on success, false otherwise
  //------------------------------------------------------------------------
  bool connectToKernel()
  {
    // create socket

    if ((_fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL)) == -1) {
      fprintf(stderr,"socket(SYSPROTO_CONTROL): %s", strerror(errno));
      return false;

    }

    // init ctl_info

    struct ctl_info ctlInfo;
    memset(&ctlInfo, 0, sizeof(ctlInfo));

    // copy name and make sure the name isn't too long

    if (strlcpy(ctlInfo.ctl_name, NET_STAT_CONTROL_NAME, sizeof(ctlInfo.ctl_name)) >=
        sizeof(ctlInfo.ctl_name)) {
      fprintf(stderr,"CONTROL NAME too long");
      return false;
    }

    // iotcl ctl info
    if (ioctl(_fd, CTLIOCGINFO, &ctlInfo)) { //} == -1) {
      fprintf(stderr,"ioctl(CTLIOCGINFO): %s", strerror(errno));
      close(_fd);
      return false;
    }

    // connect socket

    struct sockaddr_ctl sc;
    memset(&sc, 0, sizeof(sc));
    sc.sc_id = ctlInfo.ctl_id;
    sc.sc_len = sizeof(sc);
    sc.sc_family = AF_SYSTEM;
    sc.ss_sysaddr = AF_SYS_CONTROL;

    sc.sc_unit = 0 ;           /* zero means unspecified */

    if (connect(_fd, (struct sockaddr *)&sc, sizeof(sc)) != 0)
    {
      fprintf(stderr,"connect(AF_SYS_CONTROL): %s\n", strerror(errno));
    } else {
      if (_logDbg) printf("socket id:%d unit:%d\n", ctlInfo.ctl_id, sc.sc_unit);
      return true;
    }

    // no dice

    close(_fd);
    return false;
  }

  //----------------------------------------------------------
  // return true if we have a socket connection active
  //----------------------------------------------------------
  bool isConnected()
  {
    return (_fd > 0);
  }

  //----------------------------------------------------------
  // run
  // @param withCounts If true, subscribe to SRC_COUNT updates
  //----------------------------------------------------------
  void run(bool withCounts)
  {
    _withCounts = withCounts;

    if (!isConnected()) {
      printf("E run() not connected.\n"); return;
    }

    _keepRunning = true;
    unsigned int xnuVersion = getXnuVersion();

    printf("XNU version:%d\n", xnuVersion);

    if (xnuVersion > 3800)
      _structHandler = NewNTStatKernel4570();
    else if (xnuVersion > 3300)
      _structHandler = NewNTStatKernel3789();
    else if (xnuVersion > 3200)
      _structHandler = NewNTStatKernel3248();
    else if (xnuVersion > 2700)
      _structHandler = NewNTStatKernel2782();
    else
      _structHandler = NewNTStatKernel2422();


    vector<uint8_t> vec;
    _structHandler->writeAddAllTcpSrc(vec);
    SEND(vec.data(), vec.size());

    while (_keepRunning)
    {
      _readNextMessage();
    }

    close(_fd);
    _fd = 0;
  }



private:

  //---------------------------------------------------------------
  // write struct to socket fd
  //---------------------------------------------------------------
  ssize_t SEND(void *pstruct, size_t structlen)
  {
    nstat_msg_hdr* hdr = (nstat_msg_hdr*)pstruct;

    
    if (_logTrace) printf("T SEND type:%s(%d) context:%llu\n", msg_name(hdr->type).c_str(), hdr->type, hdr->context );

    ssize_t rc = write (_fd, pstruct, structlen);

    if (_logErrors && rc < structlen) printf("E ERROR on SEND write returned %d expecting %d\n", (int)rc, (int)structlen);

    return rc;
  }

  /*
  //---------------------------------------------------------------
  // write message to socket fd
  // returns true if successful
  //---------------------------------------------------------------
  bool SEND(QMsg &qm)
  {
    nstat_msg_hdr* hdr = (nstat_msg_hdr*)qm.msg.data();

    if (_logTrace) printf("T SEND type:%s(%d) context:%llu\n", msg_name(hdr->type).c_str(), hdr->type, hdr->context );

    ssize_t rc = write (_fd, qm.msg.data(), qm.msg.size());

    return (rc == qm.msg.size());
  }
  
  //----------------------------------------------------------
  //
  //----------------------------------------------------------
  void ENQ(vector<uint8_t> &vec)
  {
    nstat_msg_hdr* hdr = (nstat_msg_hdr*)vec.data();
    QMsg item = QMsg(hdr->type, hdr->context, ++_seqnum, vec);
    _outq.push_back(item);
  }

  //----------------------------------------------------------
  //
  //----------------------------------------------------------
  void sendNextMsg()
  {
    if (_outq.empty()) return;

    QMsg &qm = *_outq.begin();
    if (SEND(qm))
    {
      _qmsgMap[qm.context.to_i()] = qm;
    }
    else
    {
      // error ... drop on floor
    }
    _outq.erase(_outq.begin());
  }
*/
  
  //----------------------------------------------------------
  // removeSource
  //----------------------------------------------------------
  void _removeSource(uint64_t srcRef)
  {
    MULOCK(&_mapMutex);

    auto fit = _map.find(srcRef);
    if (fit != _map.end()) {
      _map.erase(fit);
      delete fit->second;
    }
    
    MUUNLOCK(&_mapMutex);
  }

  //----------------------------------------------------------
  // resetSource
  //----------------------------------------------------------
  void _resetSource(uint64_t srcRef, uint32_t providerId)
  {
    _removeSource(srcRef);

    NetstatSource* src = new NetstatSource(srcRef, providerId);

    MULOCK(&_mapMutex);

    _map[srcRef] = src;

    MUUNLOCK(&_mapMutex);
  }

  //----------------------------------------------------------
  // lookupSource
  //----------------------------------------------------------
  NetstatSource* _lookupSource(uint64_t srcRef)
  {
    NetstatSource* retval = 0L;

    MULOCK(&_mapMutex);

    auto fit = _map.find(srcRef);
    if (fit != _map.end()) {
      retval = fit->second;
    }

    MUUNLOCK(&_mapMutex);
    return retval;
  }

  //----------------------------------------------------------
  // Allocate new NTStreamSource and place in map
  //----------------------------------------------------------
  void onSrcAdded(uint32_t providerId, uint64_t srcRef)
  {
    _resetSource(srcRef, providerId);
  }

  //----------------------------------------------------------
  // _readNextMessage()
  // The KCQ socket is really a queue.  This reads the next
  // message off the queue
  //----------------------------------------------------------
  int _readNextMessage()
  {
    uint64_t srcRef = 0L;
    uint32_t providerId = 0;
    char c[BUFSIZE];

    int num_bytes = (int)read (_fd, c, BUFSIZE);

    if (_logDbg) printf("D READ %d bytes\n", num_bytes);

    if (num_bytes <= 0) return -1;

    nstat_msg_hdr *ns = (nstat_msg_hdr *) c;

    if (_logTrace) printf("T RECV type:%s(%d) context:%llu len:%d\n", msg_name(ns->type).c_str(), ns->type, ns->context, num_bytes);

    switch (ns->type)
    {

      case NSTAT_MSG_TYPE_SRC_ADDED:
      {
        _structHandler->getSrcRef(ns, num_bytes, srcRef, providerId);

        onSrcAdded(providerId, srcRef);

        vector<uint8_t> vec;
        _structHandler->writeSrcDesc(vec, providerId, srcRef);
        SEND(vec.data(), vec.size());

        break;
      }
      case NSTAT_MSG_TYPE_SRC_REMOVED:
      {
        _structHandler->getSrcRef(ns, num_bytes, srcRef, providerId);
        NetstatSource* source = _lookupSource(srcRef);

        if (source != 0L) {
          _listener->onStreamRemoved(&source->obj);
        }

        break;
      }
      case NSTAT_MSG_TYPE_SRC_DESC:
      {
        _structHandler->getSrcRef(ns, num_bytes, srcRef, providerId);
        NetstatSource* source = _lookupSource(srcRef);

        if (source != 0L)
        {
          if (_structHandler->readSrcDesc(ns, num_bytes, &source->obj))
          {
            if (source->obj.key.isV6 == false && source->obj.key.local.addr4.s_addr == 0) {
              printf("desc for zero address\n");
            }
            // misc cleanups

            if (source->obj.process.pid == 0) strcpy(source->obj.process.name, "kernel_task");

            // notify application

            source->_haveDesc = true;
            _listener->onStreamAdded(&source->obj);

          } else {
            if (_logDbg) printf("E not TCP or UDP provider:%u\n", providerId);
          }
        }

        break;
      }
      case NSTAT_MSG_TYPE_SRC_COUNTS:
      {
        _structHandler->getSrcRef(ns, num_bytes, srcRef, providerId);

        NetstatSource* source = _lookupSource(srcRef);
        if (source != 0L) {

          if (source->obj.key.isV6 == false && source->obj.key.local.addr4.s_addr == 0) {
            printf("counts for zero address\n");
          }
          
          _structHandler->readCounts(ns, num_bytes, source->obj.stats);
          if (source->_haveDesc) {
            
            _listener->onStreamStatsUpdate(&source->obj);
          }
        }
        break;
      }
      case NSTAT_MSG_TYPE_SUCCESS:
        if (ns->context == CONTEXT_QUERY_SRC) {
          // ?
        } else if (ns->context == CONTEXT_ADD_ALL_SRCS) {

          // now add UDP

          if (!_udpAdded) {
            _udpAdded = true;
            vector<uint8_t> vec;
            _structHandler->writeAddAllUdpSrc(vec);
            SEND(vec.data(), vec.size());
          } else {
            
            if (_withCounts) {
              if (!_gotCounts)
              {
                _gotCounts = true;
                vector<uint8_t> vec;
                _structHandler->writeQueryAllSrc(vec);
                SEND(vec.data(), vec.size());
              }
            }
          }

        } else {
          if (_logDbg) printf("E unhandled success response\n");
        }

        break;

      case NSTAT_MSG_TYPE_ERROR:
      {
        // Error message
        nstat_msg_error* perr = (nstat_msg_error*)c;
        if (true) printf("T error code:%d (0x%x) \n", perr->error, perr->error);
      }
        return 0;//-1;
        break;

      default:
        printf("E unknown message type:%d\n", ns->type);
        return -1;

    }

    return 0;
  }

  virtual void stop() { _keepRunning = false; }

  // private data members
  
  NetworkStatisticsListener*    _listener;

  map<uint64_t, NetstatSource*> _map;

  bool                          _keepRunning;

  int                           _fd;

  NTStatKernelStructHandler*    _structHandler;

  bool                          _udpAdded;
  
  bool                          _withCounts;    // application wants stat counters
  
  bool                          _gotCounts;     // have request counts

  MUTEX_T                       _mapMutex;
  /*
  vector<QMsg>                  _outq;  // messages that need to be sent

  uint16_t                      _seqnum;
  
  map<uint64_t, QMsg>           _qmsgMap; // messages waiting for response
  */

};






//----------------------------------------------------------
// Return new instance of impl
//----------------------------------------------------------
NetworkStatisticsClient* NetworkStatisticsClientNew(NetworkStatisticsListener* l)
{
  return new NetworkStatisticsClientImpl(l);
}

//----------------------------------------------------------
// getXnuVersion
//
// uname() will yield a version string like "xnu-3789.71.6~1/RELEASE_X86_64"
// This function extracts the integer after 'xnu-'.  3789 in this
// example.
//----------------------------------------------------------
unsigned int getXnuVersion()
{
   struct utsname name;

   uname (&name);

  char *p = strstr(name.version, "xnu-");
  if (0L == p) {
    // unexpected
    return 2000;
  }

  unsigned int val = (unsigned int)atol(p + 4);
  return val;
}

//----------------------------------------------------------
// string name for message type
//----------------------------------------------------------
string msg_name(uint32_t msg_type)
{
  switch(msg_type) {
    case NSTAT_MSG_TYPE_ERROR: return "ERROR";
    case NSTAT_MSG_TYPE_SUCCESS: return "SUCCESS";

    case NSTAT_MSG_TYPE_ADD_SRC: return "ADD_SRC";
    case NSTAT_MSG_TYPE_ADD_ALL_SRCS: return "ADD_ALL_SRC";
    case NSTAT_MSG_TYPE_REM_SRC: return "REM_SRC";
    case NSTAT_MSG_TYPE_QUERY_SRC: return "QUERY_SRC";
    case NSTAT_MSG_TYPE_GET_SRC_DESC: return "GET_SRC_DESC";

    case NSTAT_MSG_TYPE_SRC_ADDED: return "SRC_ADDED";
    case NSTAT_MSG_TYPE_SRC_REMOVED: return "SRC_REMOVED";
    case NSTAT_MSG_TYPE_SRC_DESC: return "SRC_DESC";
    case NSTAT_MSG_TYPE_SRC_COUNTS: return "SRC_COUNTS";
    default:
      break;
  }
  return "?";
}

//----------------------------------------------------------
// less-than operator for NTStatStreamKey
// so applications can use it in std::map
//----------------------------------------------------------
bool NTStatStreamKey::operator<(const NTStatStreamKey& b) const
{
  if (isV6 < b.isV6) return true;
  if (isV6 > b.isV6) return false;

  if (ipproto < b.ipproto) return true;
  if (ipproto > b.ipproto) return false;

  if (lport < b.lport) return true;
  if (lport > b.lport) return false;

  if (rport < b.rport) return true;
  if (rport > b.rport) return false;

  if (ifindex < b.ifindex) return true;
  if (ifindex > b.ifindex) return false;

  if (isV6)
  {
    int d = memcmp(&local.addr6, &b.local.addr6,sizeof(in6_addr));
    if (d < 0) return true;
    if (d > 0) return false;
    d = memcmp(&remote.addr6, &b.remote.addr6,sizeof(in6_addr));
    if (d < 0) return true;
    if (d > 0) return false;
  } else {
    if (local.addr4.s_addr < b.local.addr4.s_addr) return true;
    if (local.addr4.s_addr > b.local.addr4.s_addr) return false;
  }
  return false;
}
