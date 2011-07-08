/*
   LK8000 Tactical Flight Computer -  WWW.LK8000.IT
   Released under GNU/GPL License v.2
   See CREDITS.TXT file for authors and copyrights
*/

/**
 * IMI driver methods are based on the source code provided by Juraj Rojko from IMI-Gliding.
 */

#include "DevIMI.hpp"
#include "Device/Declaration.hpp"
#include "MsgParser.hpp"
#include "Device/Port.hpp"
#include "OS/Clock.hpp"

#ifdef _UNICODE
#include <windows.h>
#endif

static void
unicode2usascii(const TCHAR* unicode, char* ascii, int outSize)
{
#ifdef _UNICODE
  WideCharToMultiByte(CP_ACP, 0, unicode, -1, ascii, outSize, "?", NULL);
#else
  strncpy(ascii, unicode, outSize - 1);
  ascii[outSize - 1] = 0;
#endif
}

/* *********************** I M I    D E V I C E ************************** */

bool CDevIMI::_connected;
CDevIMI::CMsgParser CDevIMI::_parser;
CDevIMI::TDeviceInfo CDevIMI::_info;
IMI::IMIWORD CDevIMI::_serialNumber;


/**
 * @brief Calculates IMI CRC value
 *
 * @param message Message for which CRC should be provided
 * @param bytes The size of the message
 *
 * @return IMI CRC value
 */
IMI::IMIWORD CDevIMI::CRC16Checksum(const void *message, unsigned bytes)
{
  const IMIBYTE *pData = (const IMIBYTE *)message;

  IMIWORD crc = 0xFFFF;
  for(;bytes; bytes--) {
    crc  = (IMIBYTE)(crc >> 8) | (crc << 8);
    crc ^= *pData++;
    crc ^= (IMIBYTE)(crc & 0xff) >> 4;
    crc ^= (crc << 8) << 4;
    crc ^= ((crc & 0xff) << 4) << 1;
  }

  if (crc == 0xFFFF)
    crc = 0xAAAA;

  return crc;
}


/**
 * @brief Coordinates converter helper
 */
struct CDevIMI::TAngle
{
  union {
    struct {
      IMIDWORD milliminutes:16;
      IMIDWORD degrees:8;
      IMIDWORD sign:1;
    };
    IMIDWORD value;
  };

  TAngle(Angle angle) {
    sign = (angle.sign() == -1) ? 1 : 0;
    double mag = angle.magnitude_degrees();
    degrees = static_cast<IMIDWORD>(mag);
    milliminutes = static_cast<IMIDWORD>((mag - degrees) * 60 * 1000);
  }
};


/**
 * @brief Sets data in IMI Waypoint structure
 *
 * @param decl LK task declaration
 * @param imiIdx The index of IMI waypoint to set
 * @param imiWp IMI waypoint structure to set
 */
void CDevIMI::IMIWaypoint(const Declaration &decl, unsigned imiIdx, TWaypoint &imiWp)
{
  unsigned idx = imiIdx == 0 ? 0 :
    (imiIdx == decl.size() + 1 ? imiIdx - 2 : imiIdx - 1);
  const Declaration::TurnPoint &tp = decl.TurnPoints[idx];
  const Waypoint &wp = tp.waypoint;

  // set name
  unicode2usascii(wp.Name.c_str(), imiWp.name, sizeof(imiWp.name));

  // set latitude
  imiWp.lat = TAngle(wp.Location.Latitude).value;

  // set longitude
  imiWp.lon = TAngle(wp.Location.Longitude).value;

  // TAKEOFF and LANDING do not have OZs
  if(imiIdx == 0 || imiIdx == decl.size() + 1)
    return;

  // set observation zones
  if(imiIdx == 1) {
    // START
    imiWp.oz.style = 3;
    switch(tp.shape) {
    case Declaration::TurnPoint::CYLINDER: // cylinder
      imiWp.oz.A1 = 1800;
      break;
    case Declaration::TurnPoint::LINE: // line
      imiWp.oz.line_only = 1;
      break;
    case Declaration::TurnPoint::SECTOR: // fai sector
      imiWp.oz.A1 = 450;
      break;
    }
    imiWp.oz.R1 = (IMIDWORD)std::min(250000u, tp.radius);
  } else if(imiIdx == decl.size()) {
    // FINISH
    imiWp.oz.style = 4;
    switch(tp.shape) {
    case Declaration::TurnPoint::CYLINDER: // cylinder
      imiWp.oz.A1 = 1800;
      break;
    case Declaration::TurnPoint::LINE: // line
      imiWp.oz.line_only = 1;
      break;
    case Declaration::TurnPoint::SECTOR: // fai sector
      imiWp.oz.A1 = 450;
      break;
    }
    imiWp.oz.R1 = (IMIDWORD)std::min(250000u, tp.radius);
  } else {
    // TPs
    imiWp.oz.style = 2;
    switch(tp.shape) {
    case Declaration::TurnPoint::CYLINDER: // cylinder
      imiWp.oz.A1 = 1800;
      imiWp.oz.R1 = (IMIDWORD)std::min(250000u, tp.radius);
      break;
    case Declaration::TurnPoint::SECTOR: // sector
      imiWp.oz.A1 = 450;
      imiWp.oz.R1 = (IMIDWORD)std::min(250000u, tp.radius);
      break;
    case Declaration::TurnPoint::LINE: // line
      assert(0);
      break;
    }
  }

  // other unused data
  imiWp.oz.maxAlt = 0;
  imiWp.oz.reduce = 0;
  imiWp.oz.move   = 0;
}


/**
 * @brief Sends message buffer to a device
 *
 * @param port Device handle
 * @param msg IMI message to send
 *
 * @return Operation status
 */
bool CDevIMI::Send(Port &port, const TMsg &msg)
{
  return port.Write(&msg, IMICOMM_MSG_HEADER_SIZE + msg.payloadSize + 2);
}


/**
 * @brief Prepares and sends the message to a device
 *
 * @param port Device handle
 * @param msgID ID of the message to send
 * @param payload Payload buffer to use for the message
 * @param payloadSize The size of the payload buffer
 * @param parameter1 1st parameter for to put in the message
 * @param parameter2 2nd parameter for to put in the message
 * @param parameter3 3rd parameter for to put in the message
 *
 * @return Operation status
 */
bool CDevIMI::Send(Port &port,
                   IMIBYTE msgID, const void *payload /* =0 */, IMIWORD payloadSize /* =0 */,
                   IMIBYTE parameter1 /* =0 */, IMIWORD parameter2 /* =0 */, IMIWORD parameter3 /* =0 */)
{
  if(payloadSize > COMM_MAX_PAYLOAD_SIZE)
    return false;

  TMsg msg;
  memset(&msg, 0, sizeof(msg));

  msg.syncChar1 = IMICOMM_SYNC_CHAR1;
  msg.syncChar2 = IMICOMM_SYNC_CHAR2;
  msg.sn = _serialNumber;
  msg.msgID = msgID;
  msg.parameter1 = parameter1;
  msg.parameter2 = parameter2;
  msg.parameter3 = parameter3;
  msg.payloadSize = payloadSize;
  memcpy(msg.payload, payload, payloadSize);

  IMIWORD crc = CRC16Checksum(((IMIBYTE*)&msg) + 2, payloadSize + IMICOMM_MSG_HEADER_SIZE - 2);
  msg.payload[payloadSize] = (IMIBYTE)(crc >> 8);
  msg.payload[payloadSize + 1] = (IMIBYTE)crc;

  return Send(port, msg);
}


/**
 * @brief Receives a message from the device
 *
 * @param port Device handle
 * @param extraTimeout Additional timeout to wait for the message
 * @param expectedPayloadSize Expected size of the message
 *
 * @return Pointer to a message structure if expected message was received or 0 otherwise
 */
const CDevIMI::TMsg *CDevIMI::Receive(Port &port, unsigned extraTimeout,
                                      unsigned expectedPayloadSize)
{
  if(expectedPayloadSize > COMM_MAX_PAYLOAD_SIZE)
    expectedPayloadSize = COMM_MAX_PAYLOAD_SIZE;

  // set timeout
  unsigned baudrate = port.GetBaudrate();
  if (baudrate == 0)
    return NULL;

  unsigned timeout = extraTimeout + 10000 * (expectedPayloadSize + sizeof(IMICOMM_MSG_HEADER_SIZE) + 10) / baudrate;
  if(!port.SetRxTimeout(timeout))
    return NULL;

  // wait for the message
  const TMsg *msg = NULL;
  timeout += MonotonicClockMS();
  while(MonotonicClockMS() < timeout) {
    // read message
    IMIBYTE buffer[64];
    int bytesRead = port.Read(buffer, sizeof(buffer));
    if(bytesRead == 0)
      continue;
    if (bytesRead == -1)
      break;

    // parse message
    const TMsg *lastMsg = _parser.Parse(buffer, bytesRead);
    if(lastMsg) {
      // message received
      if(lastMsg->msgID == MSG_ACK_NOTCONFIG)
        Disconnect(port);
      else if(lastMsg->msgID != MSG_CFG_KEEPCONFIG)
        msg = lastMsg;

      break;
    }
  }

  // restore timeout
  if(!port.SetRxTimeout(0))
    return NULL;

  return msg;
}


/**
 * @brief Sends a message and waits for a confirmation from the device
 *
 * @param port Device handle
 * @param msgID ID of the message to send
 * @param payload Payload buffer to use for the message
 * @param payloadSize The size of the payload buffer
 * @param reMsgID Expected ID of the message to receive
 * @param retPayloadSize Expected size of the received message
 * @param parameter1 1st parameter for to put in the message
 * @param parameter2 2nd parameter for to put in the message
 * @param parameter3 3rd parameter for to put in the message
 * @param extraTimeout Additional timeout to wait for the message
 * @param retry Number of send retries
 *
 * @return Pointer to a message structure if expected message was received or 0 otherwise
 */
const CDevIMI::TMsg *CDevIMI::SendRet(Port &port,
                                      IMIBYTE msgID, const void *payload, IMIWORD payloadSize,
                                      IMIBYTE reMsgID, IMIWORD retPayloadSize,
                                      IMIBYTE parameter1 /* =0 */, IMIWORD parameter2 /* =0 */, IMIWORD parameter3 /* =0 */,
                                      unsigned extraTimeout /* =300 */, int retry /* =4 */)
{
  unsigned baudRate = port.GetBaudrate();
  if (baudRate == 0)
    return NULL;

  extraTimeout += 10000 * (payloadSize + sizeof(IMICOMM_MSG_HEADER_SIZE) + 10) / baudRate;
  while (retry--) {
    if(Send(port, msgID, payload, payloadSize, parameter1, parameter2, parameter3)) {
      const TMsg *msg = Receive(port, extraTimeout, retPayloadSize);
      if(msg && msg->msgID == reMsgID && (retPayloadSize == (IMIWORD)-1 || msg->payloadSize == retPayloadSize))
        return msg;
    }
  }

  return NULL;
}



/**
 * @brief Connects to the device
 *
 * @param port Device handle
 *
 * @return Operation status
 */
bool CDevIMI::Connect(Port &port)
{
  if (_connected)
    return true;

  memset(&_info, 0, sizeof(_info));
  _serialNumber = 0;
  _parser.Reset();

  // check connectivity
  if (!Send(port, MSG_CFG_HELLO))
    return false;

  const TMsg *msg = Receive(port, 100, 0);
  if (!msg || msg->msgID != MSG_CFG_HELLO)
    return false;

  _serialNumber = msg->sn;

  // configure baudrate
  unsigned baudRate = port.GetBaudrate();
  if (baudRate == 0)
    return false;

  if(!Send(port, MSG_CFG_STARTCONFIG, 0, 0, IMICOMM_BIGPARAM1(baudRate), IMICOMM_BIGPARAM2(baudRate)))
    return false;

  // get device info
  for (unsigned i = 0; i < 4; i++) {
    if (!Send(port, MSG_CFG_DEVICEINFO))
      continue;

    const TMsg *msg = Receive(port, 300, sizeof(TDeviceInfo));
    if (!msg)
      return false;

    if (msg->msgID != MSG_CFG_DEVICEINFO)
      continue;

    if (msg->payloadSize == sizeof(TDeviceInfo)) {
      memcpy(&_info, msg->payload, sizeof(TDeviceInfo));
    } else if (msg->payloadSize == 16) {
      // old version of the structure
      memset(&_info, 0, sizeof(TDeviceInfo));
      memcpy(&_info, msg->payload, 16);
    } else {
      return false;
    }

    _connected = true;
    return true;
  }

  return false;
}


/**
 * @brief Sends task declaration
 *
 * @param port Device handle
 * @param decl Task declaration data
 *
 * @return Operation status
 */
bool CDevIMI::DeclarationWrite(Port &port, const Declaration &decl)
{
  if (!_connected)
    return false;

  TDeclaration imiDecl;
  memset(&imiDecl, 0, sizeof(imiDecl));

  // idecl.date ignored - will be set by FR
  unicode2usascii(decl.PilotName,        imiDecl.header.plt, sizeof(imiDecl.header.plt));
  // decl.header.db1Year = year; decl.header.db1Month = month; decl.header.db1Day = day;
  unicode2usascii(decl.AircraftType,     imiDecl.header.gty, sizeof(imiDecl.header.gty));
  unicode2usascii(decl.AircraftReg,     imiDecl.header.gid, sizeof(imiDecl.header.gid));
  unicode2usascii(decl.CompetitionId,    imiDecl.header.cid, sizeof(imiDecl.header.cid));
  unicode2usascii(_T("XCSOARTASK"), imiDecl.header.tskName, sizeof(imiDecl.header.tskName));

  IMIWaypoint(decl, 0, imiDecl.wp[0]);
  for (unsigned i = 0; i < decl.size(); i++)
    IMIWaypoint(decl, i + 1, imiDecl.wp[i + 1]);
  IMIWaypoint(decl, decl.size() + 1, imiDecl.wp[decl.size() + 1]);

  // send declaration for current task
  return SendRet(port, MSG_DECLARATION, &imiDecl, sizeof(imiDecl),
                 MSG_ACK_SUCCESS, 0, -1) != NULL;
}


/**
 * @brief Disconnects from the device
 *
 * @param port Device handle
 *
 * @return Operation status
 */
bool CDevIMI::Disconnect(Port &port)
{
  if (!_connected)
    return true;

  if (!Send(port, MSG_CFG_BYE))
    return false;

  _connected = false;
  return true;
}

bool
CDevIMI::DeclareTask(Port &port, const Declaration &declaration)
{
  // verify WP number
  if (declaration.size() < 2 || declaration.size() > 13)
    return false;

  // stop Rx thread
  if(!port.StopRxThread())
    return false;

  // set new Rx timeout
  bool status = port.SetRxTimeout(2000);
  if(status) {
    // connect to the device
    status = Connect(port);
    if(status) {
      // task declaration
      status &= DeclarationWrite(port, declaration);
    }

    // disconnect
    status &= Disconnect(port);

    // restore Rx timeout (we must try that always; don't overwrite error descr)
    status &= port.SetRxTimeout(0);
  }

  // restart Rx thread
  status &= port.StartRxThread();

  return status;
}

void CDevIMI::Register()
{
  _connected = false;
}
