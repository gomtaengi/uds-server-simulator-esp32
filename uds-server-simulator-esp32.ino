#include "third/cJSON.h"
#include "third/ESP32-TWAI-CAN.hpp"
#include "third/ESP32-TWAI-CAN.cpp"
#include "uds-server-simulator.h"
#include <LittleFS.h>

// Default for ESP32
#define CAN_TX 5
#define CAN_RX 4

CanFrame rxFrame;
/* Globals */
char *version = "v1.1.2";

int io_control_id_flag = 0;  // 0-false 1-true
long io_control_seconds = 0;
long io_control_microseconds = 0;

int diag_func_req_id = 0;  // default 0
int diag_phy_req_id = 0;   // default 0
int diag_phy_resp_id = 0;  // default 0

long change_to_non_default_session_seconds = 0;       // seconds part
long change_to_non_default_session_microseconds = 0;  // microseconds part
const long S3Server_timer = 5;                        // default 5s

int current_session_mode = 1;           // default session mode(1), only support 1 and 3 in this version.
int current_security_level = 0;         // default not unlocked(0), only support 3 and 19 in this version.
int current_security_phase_3 = 0;       // default 0, there are two phases: 1 and 2.
int current_security_phase_19 = 0;      // default 0, there are two phases: 1 and 2.
int current_security_phase_21 = 0;      // default 0, there are two phases: 1 and 2.
uint8_t *current_seed_3 = NULL;         // store the current 27's seed of security level 0x03.
uint8_t *current_seed_19 = NULL;        // store the current 27's seed of security level 0x19.
uint8_t *current_seed_21 = NULL;        // store the current 27's seed of security level 0x21.
int security_access_error_attempt = 0;  // store service 27 error attempt number, only limit sl 19.

uint8_t tmp_store[8] = { 0 };

int flow_control_flag = 0;  // 0-false 1-true
uint8_t st_min = 0;         // us

/* This is for $22 flow control packets */
uint8_t gBuffer[256] = { 0 };
int gBufSize = 0;
int gBufLengthRemaining = 0;
int gBufCounter = 0;

/* This is for $2E flow control packets */
uint8_t ggBuffer[256] = { 0 };
int ggBufSize = 0;
int ggBufLengthRemaining = 0;
int ggBufCounter = 0;

/********************************** DID Supported Start **********************************/
/* DID for 22 & 2E services, write DID value without authentication */
int DID_No_Security[100] = { 0 };
int DID_No_Security_Num = 0;

/* DID for 22 & 2E services, write DID value with 27 03 */
int DID_Security_03[100] = { 0 };
int DID_Security_03_Num = 0;

/* DID for 22 & 2E services, write DID value with 27 19 */
int DID_Security_19[100] = { 0 };
int DID_Security_19_Num = 0;

/* DID for 22 & 2E services, write DID value with 27 21 */
int DID_Security_21[100] = { 0 };
int DID_Security_21_Num = 0;

/* DID number for 22 & 2E services */
int DID_NUM = 0;

/* DID key-value for 22 & 2E services */
struct DIDKeyValuePair {
  int key;
  uint8_t value[256];
} pairs[256];

/* DID for 2F service */
int DID_IO_Control[100] = { 0 };
int DID_IO_Control_Num = 0;
/********************************** DID Supported End **********************************/

void sendObdFrame(uint8_t obdId) {
  CanFrame obdFrame = { 0 };
  obdFrame.identifier = 0x7DF;  // Default OBD2 address;
  obdFrame.extd = 0;
  obdFrame.data_length_code = 8;
  obdFrame.data[0] = 2;
  obdFrame.data[1] = 1;
  obdFrame.data[2] = obdId;
  obdFrame.data[3] = 0xAA;  // Best to use 0xAA (0b10101010) instead of 0
  obdFrame.data[4] = 0xAA;  // CAN works better this way as it needs
  obdFrame.data[5] = 0xAA;  // to avoid bit-stuffing
  obdFrame.data[6] = 0xAA;
  obdFrame.data[7] = 0xAA;
  // Accepts both pointers and references
  ESP32Can.writeFrame(obdFrame);  // timeout defaults to 1 ms
}


uint8_t *seed_generate(int sl) {
  uint8_t *seed_ptr = (uint8_t *)malloc(sizeof(uint8_t) * 4);  // store 27's 4-byte seed

  if (sl == 0x03) {
    seed_ptr[0] = 0x00;
    seed_ptr[1] = 0x00;
    seed_ptr[2] = 0x00;
    seed_ptr[3] = 0x00;
    return seed_ptr;
  }

  if (sl == 0x19 || sl == 0x21) {
    uint8_t str[3];
    int ret;
    int num;
    srand((unsigned int)time(NULL));
    for (int i = 0; i < 4; i++) {
      ret = rand();
      sprintf((char *)str, "%02d", ret % 100);
      num = strtol((char *)str, NULL, 16);
      seed_ptr[i] = (uint8_t)num;
    }
    return seed_ptr;
  }
}

uint8_t *security_algorithm(uint8_t *seed_ptr, int sl) {
  uint8_t *key_ptr = (uint8_t *)malloc(sizeof(uint8_t) * 4);  // store 27's 4-byte key

  if (sl == 0x04) {
    key_ptr[0] = 0xde;
    key_ptr[1] = 0xad;
    key_ptr[2] = 0xbe;
    key_ptr[3] = 0xef;
    return key_ptr;
  }

  if (sl == 0x1A || sl == 0x22) {
    uint8_t Seed[4];
    uint8_t Const[4];
    uint8_t Key[4];
    uint32_t wConst = 0xdeadbeef;

    Seed[0] = *seed_ptr;
    Seed[1] = *(seed_ptr + 1);
    Seed[2] = *(seed_ptr + 2);
    Seed[3] = *(seed_ptr + 3);

    Const[3] = (uint8_t)((wConst & 0xff000000) >> 24);
    Const[2] = (uint8_t)((wConst & 0x00ff0000) >> 16);
    Const[1] = (uint8_t)((wConst & 0x0000ff00) >> 8);
    Const[0] = (uint8_t)(wConst & 0x000000ff);

    Key[0] = Const[0] * (Seed[0] * Seed[0]) + Const[1] * (Seed[1] * Seed[1]) + Const[2] * (Seed[0] * Seed[1]);
    Key[1] = Const[0] * (Seed[0]) + Const[1] * (Seed[1]) + Const[3] * (Seed[0] * Seed[1]);
    Key[2] = Const[0] * (Seed[2] * Seed[3]) + Const[1] * (Seed[3] * Seed[3]) + Const[2] * (Seed[2] * Seed[3]);
    Key[3] = Const[0] * (Seed[2] * Seed[3]) + Const[1] * (Seed[3]) + Const[3] * (Seed[2] * Seed[3]);

    key_ptr = Key;
    return key_ptr;
  }
}

void send_negative_response(int sid, int nrc) {
  CanFrame resp = { 0 };
  resp.identifier = diag_phy_resp_id;
  resp.data_length_code = 8;
  resp.data[0] = 0x03;
  resp.data[1] = 0x7F;
  resp.data[2] = sid;
  resp.data[3] = nrc;
  resp.data[4] = 0x00;
  resp.data[5] = 0x00;
  resp.data[6] = 0x00;
  resp.data[7] = 0x00;
  ESP32Can.writeFrame(resp);
  return;
}

void flow_control_push_to() {  // referred to Craig Smith's uds-server.
  CanFrame frame;
  frame.identifier = diag_phy_resp_id;
  while (gBufLengthRemaining > 0) {
    if (gBufLengthRemaining > 7) {
      frame.data_length_code = 8;
      frame.data[0] = gBufCounter;
      memcpy(&frame.data[1], gBuffer + (gBufSize - gBufLengthRemaining), 7);
      ESP32Can.writeFrame(frame);
      gBufCounter++;
      if (gBufCounter == 0x30)
        gBufCounter = 0x20;
      gBufLengthRemaining -= 7;
    } else {
      frame.data_length_code = 8;
      frame.data[0] = gBufCounter;
      memcpy(&frame.data[1], gBuffer + (gBufSize - gBufLengthRemaining), gBufLengthRemaining);
      gBufLengthRemaining += 1;
      for (; gBufLengthRemaining <= 7; gBufLengthRemaining++) {
        frame.data[gBufLengthRemaining] = 0x00;
      }
      ESP32Can.writeFrame(frame);
      gBufLengthRemaining = 0;
    }
    delay(st_min);
  }
  memset(gBuffer, 0, sizeof(gBuffer));  // clear did data buffer
}

void isotp_send_to(uint8_t *data, int size) {  // referred to Craig Smith's uds-server.
  // send did's data: server to client
  CanFrame frame;
  int left = size;
  int counter;
  int nbytes;

  // if (size > 4095) { // 0xFFF=4095
  //     send_negative_response(can, UDS_SID_READ_DATA_BY_ID, INCORRECT_MESSAGE_LENGTH_OR_INVALID_FORMAT);
  //     return;
  // }

  frame.identifier = diag_phy_resp_id;

  if (size < 8) {  // send single frame
    frame.data_length_code = 8;
    frame.data[0] = size;
    memcpy(&frame.data[1], data, size);
    size += 1;
    for (; size <= 7; size++) {
      frame.data[size] = 0x00;
    }
    ESP32Can.writeFrame(frame);
  } else {  // send multi frame
    frame.data_length_code = 8;
    if (size >= 0 && size <= 0xFF) {
      frame.data[0] = 0x10;
      frame.data[1] = size;
    }
    if (size >= 0x100 && size <= 0xFFF) {
      frame.data[0] = 0x10 + ((size & 0x00000F00) >> 8);
      frame.data[1] = (size & 0x000000FF);
    }
    memcpy(&frame.data[2], data, 6);
    ESP32Can.writeFrame(frame);
    left -= 6;
    counter = 0x21;

    memcpy(gBuffer, data, size);  // Size is restricted to < 4096
    gBufSize = size;
    gBufLengthRemaining = left;
    gBufCounter = counter;

    flow_control_flag = 1;
  }
}

unsigned int get_did_from_frame(CanFrame frame) {
  char first_char = int2nibble(frame.data[0], 0);
  int did_high_byte, did_low_byte;
  if (first_char == '0') {
    did_high_byte = frame.data[2];
    did_low_byte = frame.data[3];
  }
  if (first_char == '1') {
    did_high_byte = frame.data[3];
    did_low_byte = frame.data[4];
  }
  unsigned char bytes[] = { did_high_byte, did_low_byte };  //	two bytes of DID
  unsigned int did = (bytes[0] << 8) | bytes[1];            //	DID hex int value
  return did;
}


/********************************** uds_server_init Start **********************************/
void isJsonObjNull(cJSON *object, char *str) {
  if (object == NULL) {
    printf("## THE %s IS NOT EXISTED ##\n", str);
    exit(1);
  }
}

void isValueJsonString(cJSON *object) {
  if (object->type != cJSON_String) {
    printf("## PLEASE CONFIG %s's VALUE TO STRING TYPE ##\n", object->string);
    exit(1);
  }
}

int set_diag_id(cJSON *items, char *key_name) {
  cJSON *diag_id_s = cJSON_GetObjectItem(items, key_name);
  isJsonObjNull(diag_id_s, key_name);
  isValueJsonString(diag_id_s);
  // printf("%#x\n", strtol(diag_id_s->valuestring, NULL, 16));
  if (diag_id_s->valuestring == "") {
    return 0;
  }
  return strtol(diag_id_s->valuestring, NULL, 16);
}

int DID_assignment(cJSON *items, char *key_name, int *DID_Arrary) {
  int i = 0;
  static int k = 0;
  cJSON *DID_struct = cJSON_GetObjectItem(items, key_name);
  if (DID_struct == NULL)
    return i;

  if (DID_struct->type == cJSON_Object) {
    cJSON *obj = DID_struct->child;
    while (obj) {
      isJsonObjNull(obj, NULL);
      isValueJsonString(obj);
      DID_Arrary[i] = strtol(obj->string, NULL, 16);
      i++;
      pairs[k].key = 0;
      memset(pairs[k].value, 0, sizeof(pairs[k].value));
      pairs[k].key = strtol(obj->string, NULL, 16);
      memcpy(pairs[k].value, obj->valuestring, strlen(obj->valuestring));
      k++;
      obj = obj->next;
      // printf("%x\n", DID_Arrary[i-1]);
      // printf(":%#x :%s :%d :%d\n", pairs[k-1].key, pairs[k-1].value, k-1, i-1);
      // printf("#################################################\n");
    }
    return i;
  }

  if (DID_struct->type == cJSON_Array) {
    cJSON *arr = DID_struct->child;
    while (arr) {
      isJsonObjNull(arr, NULL);
      isValueJsonString(arr);
      DID_Arrary[i] = strtol(arr->valuestring, NULL, 16);
      i++;
      arr = arr->next;
      // printf("%x\n", DID_Arrary[i-1]);
    }
    return i;
  }

  printf("## the format of %s's value is not right. ##\n", DID_struct->string);
  exit(1);
}

// char lower2upper(char ch){
//     if((ch >= 97) && (ch <= 122))   // lower character
//         return ch ^ 32;
//     else
//         return ch;
// }

void uds_server_init(cJSON *root, char *ecu) {
  char *current_ecu = NULL;
  if (ecu != NULL) {
    current_ecu = ecu;
  } else {
    cJSON *CURRENT_ECU = cJSON_GetObjectItem(root, "CURRENT_ECU");
    isJsonObjNull(CURRENT_ECU, "CURRENT_ECU");
    isValueJsonString(CURRENT_ECU);
    current_ecu = CURRENT_ECU->valuestring;
  }

  // for(int i=0; i<strlen(current_ecu); i++){
  //     *(current_ecu+i) = lower2upper(*(current_ecu+i));
  // }

  cJSON *items = cJSON_GetObjectItem(root, current_ecu);
  isJsonObjNull(items, current_ecu);

  diag_func_req_id = set_diag_id(items, "func_req_id");
  diag_phy_req_id = set_diag_id(items, "phy_req_id");
  diag_phy_resp_id = set_diag_id(items, "phy_resp_id");
  if (diag_phy_req_id == 0 || diag_phy_resp_id == 0) {
    printf("## PLEASE SET CORRECT DIAG REQ & RESP ID ##\n");
    exit(1);
  }

  DID_No_Security_Num = DID_assignment(items, "DID_No_Security", DID_No_Security);
  DID_Security_03_Num = DID_assignment(items, "DID_Security_03", DID_Security_03);
  DID_Security_19_Num = DID_assignment(items, "DID_Security_19", DID_Security_19);
  DID_Security_21_Num = DID_assignment(items, "DID_Security_21", DID_Security_21);
  DID_NUM = (DID_No_Security_Num + DID_Security_03_Num + DID_Security_19_Num + DID_Security_21_Num);

  DID_IO_Control_Num = DID_assignment(items, "DID_IO_Control", DID_IO_Control);
}
/********************************** uds_server_init End **********************************/

void reset_relevant_variables() {  // when session mode changed
  current_security_level = 0;
  current_security_phase_3 = 0;
  current_security_phase_19 = 0;
  current_security_phase_21 = 0;
  current_seed_3 = NULL;
  current_seed_19 = NULL;
  current_seed_21 = NULL;
  security_access_error_attempt = 0;
  flow_control_flag = 0;
  st_min = 0;
  gBufSize = 0;
  gBufLengthRemaining = 0;
  gBufCounter = 0;
  memset(gBuffer, 0, sizeof(gBuffer));
  ggBufSize = 0;
  ggBufLengthRemaining = 0;
  ggBufCounter = 0;
  memset(ggBuffer, 0, sizeof(ggBuffer));
  memset(tmp_store, 0, sizeof(tmp_store));
}

char int2nibble(int two_char, int position) {
  if (position != 0 && position != 1)
    return NULL;
  char str[2];
  sprintf(str, "%02x", two_char);
  return str[position];
}

int isSFExisted(int sid, int sf) {
  int nrc_12 = isSubFunctionSupported(sid, sf);
  if (nrc_12 != 0) {
    send_negative_response(sid, nrc_12);
    return -1;
  }
  return 0;
}

/********************************** NRC Handle Start **********************************/
/* NRC 0x12 SubFunctionNotSupported */
int isSubFunctionSupported(int sid, int sf) {
  switch (sid) {
    case UDS_SID_DIAGNOSTIC_CONTROL:
      if (sf == 0x01 || sf == 0x03)
        return 0;
      break;
    case UDS_SID_TESTER_PRESENT:
      if (sf == 0x00 || sf == 0x80)
        return 0;
      break;
    case UDS_SID_SECURITY_ACCESS:
      if (sf == 0x03 || sf == 0x04 || sf == 0x19 || sf == 0x1A || sf == 0x21 || sf == 0x22)
        return 0;
      break;
    case UDS_SID_READ_DATA_BY_ID:   // 0x22 No SF
    case UDS_SID_WRITE_DATA_BY_ID:  // 0x2E No SF
    case UDS_SID_IO_CONTROL_BY_ID:  // 0x2F No SF
      return 0;
    default:
      printf("please add the new SID in the switch-case statement.\n");
      break;
  }
  return SUB_FUNCTION_NOT_SUPPORTED;
}

/* NRC 0x13 incorrectMessageLengthOrInvalidFormat */
int isIncorrectMessageLengthOrInvalidFormat(CanFrame frame) {
  int first_byte = frame.data[0];
  char first_char = int2nibble(first_byte, 0);
  char second_char = int2nibble(first_byte, 1);
  int len = sizeof(frame.data) / sizeof(frame.data[0]);
  int sid;

  // printf("%d\n", frame.can_dlc);
  // printf("%d %d %d\n", len, sizeof(frame.data), sizeof(frame.data[0]));

  if (frame.data_length_code != 8)  // must padding to 8 bytes
    return -1;
  if (first_char == '0') {  // single frame 0x00-0x07
    sid = frame.data[1];
    switch (sid) {
      case UDS_SID_DIAGNOSTIC_CONTROL:
      case UDS_SID_TESTER_PRESENT:
        if (first_byte == 0x02)
          return 0;
        break;
      case UDS_SID_SECURITY_ACCESS:  // not support the field "securityAccessDataRecord" in this verison
        if (first_byte == 0x02 || first_byte == 0x06)
          return 0;
        break;
      case UDS_SID_READ_DATA_BY_ID:  // only support to read a DID per request frame in this version
        if (first_byte == 0x03)
          return 0;
        break;
      case UDS_SID_WRITE_DATA_BY_ID:  // support write single frame
        if (first_byte >= 0x04 && first_byte <= 0x07)
          return 0;
        break;
      case UDS_SID_IO_CONTROL_BY_ID:  // only support one parameter(currentStatus) in this version
        if (first_byte >= 0x04 && first_byte <= 0x05)
          return 0;
        break;
      default:
        if (first_byte >= 0x02 && first_byte <= 0x07)
          return 0;
        break;
    }
    return INCORRECT_MESSAGE_LENGTH_OR_INVALID_FORMAT;
  }
  if (first_char == '1') {  // first frame 0x10-0x1f
    sid = frame.data[2];
    switch (sid) {
      case UDS_SID_DIAGNOSTIC_CONTROL:
      case UDS_SID_TESTER_PRESENT:
      case UDS_SID_SECURITY_ACCESS:   // not support the field "securityAccessDataRecord" in this verison
      case UDS_SID_READ_DATA_BY_ID:   // not support to read serval DIDs per request frame in this version
      case UDS_SID_IO_CONTROL_BY_ID:  // not support to control multiple parameter per request frame in this version
        return INCORRECT_MESSAGE_LENGTH_OR_INVALID_FORMAT;
      case UDS_SID_WRITE_DATA_BY_ID:  // support write multiple frame
        if (frame.data[1] >= 0x07 && frame.data[1] <= 0xFF) {
          return 0;
        } else {
          return INCORRECT_MESSAGE_LENGTH_OR_INVALID_FORMAT;
        }
      default:
        return 0;
    }
  }
  if (first_char == '2') {  // consecutive frame 0x20-0x2f
    return 0;
  }
  if (first_char == '3') {  // flow control 0x30-0x32
    if (first_byte >= 0x30 && first_byte <= 0x32) {
      return 0;
    }
  }
  return -1;
}

/* NRC 0x31 requestOutOfRange */
int isRequestOutOfRange(unsigned int did) {
  for (int i = 0; i < DID_No_Security_Num; i++) {
    if (did == DID_No_Security[i])
      return 0;
  }
  for (int j = 0; j < DID_Security_03_Num; j++) {
    if (did == DID_Security_03[j])
      return 0;
  }
  for (int k = 0; k < DID_Security_19_Num; k++) {
    if (did == DID_Security_19[k])
      return 0;
  }
  for (int k = 0; k < DID_Security_21_Num; k++) {
    if (did == DID_Security_21[k])
      return 0;
  }
  for (int l = 0; l < DID_IO_Control_Num; l++) {
    if (did == DID_IO_Control[l])
      return 0;
  }
  return REQUEST_OUT_OF_RANGE;
}

/* NRC 0x33 securityAccessDenied */
int isSecurityAccessDenied(unsigned int did) {
  for (int i = 0; i < DID_No_Security_Num; i++) {
    if (did == DID_No_Security[i])
      return 0;
  }
  for (int j = 0; j < DID_Security_03_Num; j++) {
    if (did == DID_Security_03[j] && current_security_level != 0x00)
      return 0;
  }
  for (int k = 0; k < DID_Security_19_Num; k++) {
    if (did == DID_Security_19[k] && current_security_level == 0x19)
      return 0;
  }
  for (int k = 0; k < DID_Security_21_Num; k++) {
    if (did == DID_Security_21[k] && current_security_level == 0x21)
      return 0;
  }
  for (int l = 0; l < DID_IO_Control_Num; l++) {
    if (did == DID_IO_Control[l] && current_security_level != 0x00)
      return 0;
  }
  return SECURITY_ACCESS_DENIED;
}

int isNonDefaultModeTimeout(int sid) {
  int nrc_7F = isServiceNotSupportedInActiveSession();
  if (nrc_7F != 0) {
    current_session_mode = 0x01;
    reset_relevant_variables();
    send_negative_response(sid, nrc_7F);
    return -1;
  }
  return 0;
}

int isRequestSecurityAccessMember(unsigned int did) {
  for (int k = 0; k < DID_Security_21_Num; k++) {
    if (did == DID_Security_21[k])
      return 0;
  }
  return -1;
}

/* NRC 0x7F serviceNotSupportedInActiveSession */
/* this function can only be used to the non-default session mode's services */
int isServiceNotSupportedInActiveSession() {
  struct timeval currentTime;
  gettimeofday(&currentTime, NULL);
  long currect_seconds = currentTime.tv_sec;
  long current_microseconds = currentTime.tv_usec;
  // printf("%d %d\n", currect_seconds, current_microseconds);
  // printf("%d %d\n", change_to_non_default_session_seconds, change_to_non_default_session_microseconds);
  // services not supported in the non-default session mode
  if (change_to_non_default_session_seconds == 0 || change_to_non_default_session_microseconds == 0)
    return SERVICE_NOT_SUPPORTED_IN_ACTIVE_SESSION;

  long delta_seconds = currect_seconds - change_to_non_default_session_seconds;
  long delta_microseconds = current_microseconds - change_to_non_default_session_microseconds;
  // printf("%d %d\n", delta_seconds, delta_microseconds);
  // not timeout
  if ((delta_seconds >= 0 && delta_seconds < S3Server_timer) || (delta_seconds == S3Server_timer && delta_microseconds <= 0))
    return 0;

  return SERVICE_NOT_SUPPORTED_IN_ACTIVE_SESSION;
}
/********************************** NRC Handle End **********************************/

/********************************** Service Implement Start **********************************/
void session_mode_change(CanFrame frame) {
  CanFrame resp = { 0 };
  resp.identifier = diag_phy_resp_id;
  resp.data_length_code = 8;
  resp.data[0] = 0x06;
  resp.data[1] = frame.data[1] + 0x40;
  resp.data[2] = frame.data[2];
  resp.data[3] = 0x00;
  resp.data[4] = 0x32;
  resp.data[5] = 0x01;
  resp.data[6] = 0xF4;
  resp.data[7] = 0x00;
  ESP32Can.writeFrame(resp);
  if (current_session_mode != frame.data[2]) {
    current_session_mode = frame.data[2];
    reset_relevant_variables();
  }
  if (current_session_mode == 0x01) {  // init defauit session time
    change_to_non_default_session_seconds = 0;
    change_to_non_default_session_microseconds = 0;
  } else {  // init non-defauit session time
    struct timeval currentTime;
    gettimeofday(&currentTime, NULL);
    change_to_non_default_session_seconds = currentTime.tv_sec;
    change_to_non_default_session_microseconds = currentTime.tv_usec;
  }
}

void tester_present(CanFrame frame) {
  struct timeval currentTime;
  gettimeofday(&currentTime, NULL);
  change_to_non_default_session_seconds = currentTime.tv_sec;
  change_to_non_default_session_microseconds = currentTime.tv_usec;

  int sf = frame.data[2];
  if (sf == 0x00) {  // must give a response
    CanFrame resp = { 0 };
    resp.identifier = diag_phy_resp_id;
    resp.data_length_code = 8;
    resp.data[0] = 0x02;
    resp.data[1] = frame.data[1] + 0x40;
    resp.data[2] = frame.data[2];
    resp.data[3] = 0x00;
    resp.data[4] = 0x00;
    resp.data[5] = 0x00;
    resp.data[6] = 0x00;
    resp.data[7] = 0x00;
    ESP32Can.writeFrame(resp);
    return;
  }
  if (sf == 0x80) {  // do not give a response
    return;
  }
}

void read_data_by_id(CanFrame frame) {
  // reset relevant buffer
  memset(gBuffer, 0, sizeof(gBuffer));
  gBufSize = 0;
  gBufLengthRemaining = 0;
  gBufCounter = 0;

  unsigned int did = get_did_from_frame(frame);
  int nrc_31 = isRequestOutOfRange(did);
  if (nrc_31 != 0) {
    send_negative_response(UDS_SID_READ_DATA_BY_ID, nrc_31);
    return;
  }

  if (isRequestSecurityAccessMember(did) == 0) {
    int nrc_33 = isSecurityAccessDenied(did);
    if (nrc_33 != 0) {
      send_negative_response(UDS_SID_WRITE_DATA_BY_ID, nrc_33);
      return;
    }
  }

  uint8_t str[256] = { 0 };
  str[0] = 0x62;
  str[1] = (uint8_t)((did & 0x0000ff00) >> 8);
  str[2] = (uint8_t)(did & 0x000000ff);
  for (int i = 0; i < DID_NUM; i++) {
    if (pairs[i].key == did) {
      memcpy(&str[3], pairs[i].value, strlen((char *)pairs[i].value));
      break;
    }
  }
  isotp_send_to(str, strlen((char *)str));
  memset(str, 0, sizeof(str));
}

void write_data_by_id(CanFrame frame) {
  // reset relevant buffer
  memset(ggBuffer, 0, sizeof(ggBuffer));
  ggBufSize = 0;
  ggBufLengthRemaining = 0;
  ggBufCounter = 0;

  unsigned int did = get_did_from_frame(frame);
  int nrc_31 = isRequestOutOfRange(did);
  if (nrc_31 != 0) {
    send_negative_response(UDS_SID_WRITE_DATA_BY_ID, nrc_31);
    return;
  }
  int nrc_33 = isSecurityAccessDenied(did);
  if (nrc_33 != 0) {
    send_negative_response(UDS_SID_WRITE_DATA_BY_ID, nrc_33);
    return;
  }

  CanFrame resp;
  char first_char = int2nibble(frame.data[0], 0);
  if (first_char == '0') {

    for (int i = 0; i < DID_NUM; i++) {
      if (pairs[i].key == did) {
        memset(pairs[i].value, 0, sizeof(pairs[i].value));  // clear the original value
        memcpy(pairs[i].value, &frame.data[4], frame.data[0] - 3);
      }
    }

    resp.identifier = diag_phy_resp_id;
    resp.data_length_code = 8;
    resp.data[0] = 0x03;
    resp.data[1] = frame.data[1] + 0x40;
    resp.data[2] = frame.data[2];
    resp.data[3] = frame.data[3];
    resp.data[4] = 0x00;
    resp.data[5] = 0x00;
    resp.data[6] = 0x00;
    resp.data[7] = 0x00;
    ESP32Can.writeFrame(resp);
  }
  if (first_char == '1') {
    ggBufSize = ((frame.data[0] & 0x0000000F) << 8) | frame.data[1];
    ggBufSize -= 3;
    ggBufCounter = 0x21;
    ggBufLengthRemaining = ggBufSize - 3;
    memset(ggBuffer, 0, sizeof(ggBuffer));  // clear the original value
    memcpy(ggBuffer, &frame.data[5], 3);

    tmp_store[0] = frame.data[2];  // SID
    tmp_store[1] = frame.data[3];  // DID_H
    tmp_store[2] = frame.data[4];  // DID_L

    resp.identifier = diag_phy_resp_id;
    resp.data_length_code = 8;
    resp.data[0] = 0x30;
    resp.data[1] = 0x00;
    resp.data[2] = 0x0F;
    resp.data[3] = 0x00;
    resp.data[4] = 0x00;
    resp.data[5] = 0x00;
    resp.data[6] = 0x00;
    resp.data[7] = 0x00;
    ESP32Can.writeFrame(resp);
  }
}

void security_access(CanFrame frame) {
  int sl = frame.data[2];
  uint8_t *seedp;
  uint8_t *keyp;
  CanFrame resp;

  /* 27 first phase: request a 4-byte seed from server */
  if (sl == 0x03 || sl == 0x19 || sl == 0x21) {
    seedp = seed_generate(sl);
    resp.identifier = diag_phy_resp_id;
    resp.data_length_code = 8;
    resp.data[0] = 0x06;
    resp.data[1] = frame.data[1] + 0x40;
    resp.data[2] = frame.data[2];
    resp.data[3] = *seedp;
    resp.data[4] = *(seedp + 1);
    resp.data[5] = *(seedp + 2);
    resp.data[6] = *(seedp + 3);
    resp.data[7] = 0x00;
    ESP32Can.writeFrame(resp);
    if (sl == 0x03) {
      current_seed_3 = seedp;
      current_security_phase_3 = 1;
    }
    if (sl == 0x19) {
      current_seed_19 = seedp;
      current_security_phase_19 = 1;
    }
    if (sl == 0x21) {
      current_seed_21 = seedp;
      current_security_phase_21 = 1;
    }
    return;
  }
  if (sl == 0x04 || sl == 0x1A || sl == 0x22) {
    /* must request seed firstly */
    if ((sl == 0x04 && current_security_phase_3 != 1) || (sl == 0x1A && current_security_phase_19 != 1) || (sl == 0x22 && current_security_phase_21 != 1)) {
      send_negative_response(UDS_SID_SECURITY_ACCESS, REQUEST_SEQUENCE_ERROR);
      return;
    }

    /* calculate key */
    if (sl == 0x04 && current_seed_3 != NULL && current_security_phase_3 == 1) {
      keyp = security_algorithm(current_seed_3, sl);
      // printf("%02x %02x %02x %02x\n", keyp[0], keyp[1], keyp[2], keyp[3]);
    }
    if (sl == 0x1A && current_seed_19 != NULL && current_security_phase_19 == 1) {
      keyp = security_algorithm(current_seed_19, sl);
    }
    if (sl == 0x22 && current_seed_21 != NULL && current_security_phase_21 == 1) {
      keyp = security_algorithm(current_seed_21, sl);
    }
    /* determine the passed key is right or not */
    if (*keyp == frame.data[3] && *(keyp + 1) == frame.data[4]
        && *(keyp + 2) == frame.data[5] && *(keyp + 3) == frame.data[6]) {  // key is correct
      resp.identifier = diag_phy_resp_id;
      resp.data_length_code = 8;
      resp.data[0] = 0x02;
      resp.data[1] = frame.data[1] + 0x40;
      resp.data[2] = frame.data[2];
      resp.data[3] = 0x00;
      resp.data[4] = 0x00;
      resp.data[5] = 0x00;
      resp.data[6] = 0x00;
      resp.data[7] = 0x00;
      ESP32Can.writeFrame(resp);
      if (sl == 0x04) {
        current_security_level = sl - 1;
        current_security_phase_3 = 2;
      }
      if (sl == 0x1A) {
        current_security_level = sl - 1;
        current_security_phase_19 = 2;
        security_access_error_attempt = 0;
      }
      if (sl == 0x22) {
        current_security_level = sl - 1;
        current_security_phase_21 = 2;
        security_access_error_attempt = 0;
      }
    } else {  // key is incorrect
      send_negative_response(UDS_SID_SECURITY_ACCESS, INVALID_KEY);
      if (sl == 0x04) {
        current_security_phase_3 = 0;
      }
      if (sl == 0x1A) {
        current_security_phase_19 = 0;
        security_access_error_attempt += 1;
      }
      if (sl == 0x22) {
        current_security_phase_21 = 0;
        security_access_error_attempt += 1;
      }
    }
    /* determine the service 27 error attempt number is exceed or not */
    if (security_access_error_attempt >= SECURITY_ACCESS_ERROR_LIMIT_NUM) {
      send_negative_response(UDS_SID_SECURITY_ACCESS, EXCEED_NUMBER_OF_ATTEMPTS);
    }
    /* reset the global variables current_seed_* */
    if (sl == 0x04) {
      current_seed_3 = NULL;
    }
    if (sl == 0x1A) {
      current_seed_19 = NULL;
    }
    if (sl == 0x22) {
      current_seed_21 = NULL;
    }
    return;
  }
}

void io_control_by_did(CanFrame frame) {
  unsigned int did = get_did_from_frame(frame);
  int nrc_31 = isRequestOutOfRange(did);
  if (nrc_31 != 0) {
    send_negative_response(UDS_SID_IO_CONTROL_BY_ID, nrc_31);
    return;
  }
  int nrc_33 = isSecurityAccessDenied(did);
  if (nrc_33 != 0) {
    send_negative_response(UDS_SID_IO_CONTROL_BY_ID, nrc_33);
    return;
  }

  int iocp = frame.data[4];
  int cs = frame.data[5];
  switch (iocp) {
    case 0x03:  // only support function "shortTermAdjustment" in this version.
      if (did == 0xF081) {
        if (cs <= 0 || cs >= 7) {
          send_negative_response(UDS_SID_IO_CONTROL_BY_ID, REQUEST_OUT_OF_RANGE);
          return;
        }
        struct timeval currentTime;
        gettimeofday(&currentTime, NULL);
        io_control_seconds = currentTime.tv_sec;
        io_control_microseconds = currentTime.tv_usec;
        tmp_store[0] = did & 0x000000FF;
        tmp_store[1] = (did & 0x0000FF00) >> 8;
        tmp_store[2] = cs;
        io_control_id_flag = 1;
      }
      return;
    default:  // do nothing
      return;
  }
}
/********************************** Service Implement End **********************************/

void setup() {
  // Setup serial for debbuging.
  Serial.begin(115200);

  if (!LittleFS.begin(true)) {
    Serial.println("An Error has occurred while mounting LittleFS");
  }
  Serial.println("LittleFS mounted successfully");
  File file = LittleFS.open(CONFIG_JSON_FILE, "r");
  if (!file) {
    Serial.println("Please upload the config.json first.");
    return;
  } else {
    Serial.println("Initing...");
    cJSON *root = read_config();
    if (!root) {
      Serial.println("Error in read_config");
    }
    uds_server_init(root, NULL);
  }

  // Set pins
  ESP32Can.setPins(CAN_TX, CAN_RX);

  // You can set custom size for the queues - those are default
  ESP32Can.setRxQueueSize(5);
  ESP32Can.setTxQueueSize(5);

  // .setSpeed() and .begin() functions require to use TwaiSpeed enum,
  // but you can easily convert it from numerical value using .convertSpeed()
  ESP32Can.setSpeed(ESP32Can.convertSpeed(500));

  // You can also just use .begin()..
  if (ESP32Can.begin()) {
    Serial.println("CAN bus started!");
  } else {
    Serial.println("CAN bus failed!");
  }

  // or override everything in one command;
  // It is also safe to use .begin() without .end() as it calls it internally
  if (ESP32Can.begin(ESP32Can.convertSpeed(500), CAN_TX, CAN_RX, 10, 10)) {
    Serial.println("CAN bus started!");
  } else {
    Serial.println("CAN bus failed!");
  }
}

cJSON *read_config() {
  File file = LittleFS.open(CONFIG_JSON_FILE, "r");
  if (file == NULL) {
    Serial.println("## OPEN CONFIG FAIL ##");
    return NULL;
  }
  int fileSize = file.size();
  char *jsonStr = (char *)malloc(sizeof(char) * fileSize + 1);
  memset(jsonStr, 0, fileSize + 1);
  file.readBytes(jsonStr, fileSize);
  jsonStr[fileSize] = '\0';
  file.close();

  cJSON *root = cJSON_Parse(jsonStr);
  if (!root) {
    const char *err = cJSON_GetErrorPtr();
    Serial.printf("Error before: [%s]\n", err);
    free((void *)err);
    free(jsonStr);
    return NULL;
  }
  free(jsonStr);
  return root;
}

void handle_pkt(CanFrame frame) {
  /* DO NOT RECEIVE OTHER CANID */
  if (frame.identifier != diag_func_req_id && frame.identifier != diag_phy_req_id) {
    Serial.printf("Not REQID, Exitting... PHY_REQID: %x\n", diag_phy_req_id);
    return;
  }
  /* GET SID & SF */
  char first_char = int2nibble(frame.data[0], 0);
  Serial.printf("first_char: %c\n", first_char);
  int sid, sf;
  if (first_char == '0') {
    sid = frame.data[1];
    sf = frame.data[2];
  }
  if (first_char == '1') {
    sid = frame.data[2];
    sf = frame.data[3];
  }
  // NRC 0x13
  int nrc_13 = isIncorrectMessageLengthOrInvalidFormat(frame);
  if (nrc_13 == -1)  // do not give a response
    return;
  if (nrc_13 != 0) {
    send_negative_response(sid, nrc_13);
    return;
  }

  if (first_char == '0' || first_char == '1') {
    switch (sid) {
      case UDS_SID_DIAGNOSTIC_CONTROL:  // SID 0x10
        if (isSFExisted(sid, sf) == -1)
          return;
        session_mode_change(frame);
        return;
      case UDS_SID_TESTER_PRESENT:  // SID 0x3E
        if (current_session_mode != 0x01) {
          if (isSFExisted(sid, sf) == -1)
            return;
          tester_present(frame);
          return;
        } else {  // default session mode
          send_negative_response(sid, SERVICE_NOT_SUPPORTED_IN_ACTIVE_SESSION);
          return;
        }
      case UDS_SID_READ_DATA_BY_ID:  // SID 0x22
        if (isSFExisted(sid, sf) == -1)
          return;
        read_data_by_id(frame);
        return;
      case UDS_SID_WRITE_DATA_BY_ID:         // SID 0x2E
        if (current_session_mode != 0x01) {  // only support SID 0x2E in non-default session mode
          if (isNonDefaultModeTimeout(sid) == -1)
            return;
          if (isSFExisted(sid, sf) == -1)
            return;
          write_data_by_id(frame);
          return;
        } else {  // default session mode
          send_negative_response(sid, SERVICE_NOT_SUPPORTED_IN_ACTIVE_SESSION);
          return;
        }
      case UDS_SID_SECURITY_ACCESS:          // SID 0x27
        if (current_session_mode != 0x01) {  // only support SID 0x27 in non-default session mode
          // printf("## current_session_mode = %#x\n", current_session_mode);
          if (isNonDefaultModeTimeout(sid) == -1)
            return;
          if (isSFExisted(sid, sf) == -1)
            return;
          security_access(frame);
          return;
        } else {  // default session mode
          send_negative_response(sid, SERVICE_NOT_SUPPORTED_IN_ACTIVE_SESSION);
          return;
        }
      case UDS_SID_IO_CONTROL_BY_ID:         // SID 0X2F
        if (current_session_mode != 0x01) {  // only support SID 0x2F in non-default session mode
          if (isNonDefaultModeTimeout(sid) == -1)
            return;
          if (isSFExisted(sid, sf) == -1)
            return;
          io_control_by_did(frame);
          return;
        } else {  // default session mode
          send_negative_response(sid, SERVICE_NOT_SUPPORTED_IN_ACTIVE_SESSION);
          return;
        }
      default:
        send_negative_response(sid, SERVICE_NOT_SUPPORTED);
        return;
    }
  }

  if (first_char == '2') {
    if (ggBufLengthRemaining > 0) {
      if (isNonDefaultModeTimeout(tmp_store[0]) == -1) {
        return;
      }

      if (ggBufCounter != frame.data[0]) {
        memset(ggBuffer, 0, sizeof(ggBuffer));
        return;
      }

      if (ggBufLengthRemaining > 7) {
        memcpy(ggBuffer + (ggBufSize - ggBufLengthRemaining), &frame.data[1], 7);
        ggBufCounter++;
        if (ggBufCounter == 0x30)
          ggBufCounter = 0x20;
        ggBufLengthRemaining -= 7;
      } else {
        memcpy(ggBuffer + (ggBufSize - ggBufLengthRemaining), &frame.data[1], ggBufLengthRemaining);
        ggBufLengthRemaining = 0;
        unsigned char bytes[] = { tmp_store[1], tmp_store[2] };  //	two bytes of DID
        unsigned int did = (bytes[0] << 8) | bytes[1];           //	DID hex int value
        for (int i = 0; i < DID_NUM; i++) {
          if (pairs[i].key == did) {
            memset(pairs[i].value, 0, sizeof(pairs[i].value));  // clear the original value
            memcpy(pairs[i].value, ggBuffer, strlen((char *)ggBuffer));
          }
        }
        CanFrame resp;
        resp.identifier = diag_phy_resp_id;
        resp.data_length_code = 8;
        resp.data[0] = 0x03;
        resp.data[1] = tmp_store[0] + 0x40;
        resp.data[2] = tmp_store[1];
        resp.data[3] = tmp_store[2];
        resp.data[4] = 0x00;
        resp.data[5] = 0x00;
        resp.data[6] = 0x00;
        resp.data[7] = 0x00;
        ESP32Can.writeFrame(resp);
        memset(tmp_store, 0, sizeof(tmp_store));
        memset(ggBuffer, 0, sizeof(ggBuffer));
      }
    }
    return;
  }

  if (first_char == '3') {
    if (flow_control_flag) {  // 0-false 1-true
      // uint8_t FS = frame.data[0] & 0x0F;
      // uint8_t BS = frame.data[1];
      uint8_t STmin = frame.data[2];

      // fs = FS;
      // bs = BS;
      if (STmin > 0 && STmin <= 0x7F) {  // 0x0-0x7F ms
        st_min = STmin * 1000;
      } else if (STmin >= 0xF1 && STmin <= 0xF9) {  // 100-900 us
        st_min = (STmin & 0x0F) * 100;
      } else {  // 1 ms
        st_min = 1000;
      }

      flow_control_push_to();
      flow_control_flag = 0;
    }
  }
}

void loop() {
  static uint32_t lastStamp = 0;
  uint32_t currentStamp = millis();

  if (currentStamp - lastStamp > 1000) {  // sends OBD2 request every second
    // TODO: Generate random CAN data
    lastStamp = currentStamp;
    sendObdFrame(5);  // For coolant temperature
  }

  // You can set custom timeout, default is 1000
  if (ESP32Can.readFrame(rxFrame, 1000)) {
    // Comment out if too many requests
    handle_pkt(rxFrame);
  }
}