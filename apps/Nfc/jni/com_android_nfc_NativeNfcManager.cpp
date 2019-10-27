/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>

#include "com_android_nfc.h"

#define ERROR_BUFFER_TOO_SMALL       -12
#define ERROR_INSUFFICIENT_RESOURCES -9
#define EEDATA_SETTINGS_NUMBER       22

static phLibNfc_sConfig_t   gDrvCfg;
static void                 *gHWRef;
static phNfc_sData_t gInputParam;
static phNfc_sData_t gOutputParam;

static phLibNfc_Handle              hLlcpHandle;
static NFCSTATUS                    lastErrorStatus = NFCSTATUS_FAILED;
static phLibNfc_Llcp_eLinkStatus_t  g_eLinkStatus = phFriNfc_LlcpMac_eLinkDefault;

static sem_t *nfc_jni_manager_sem;
static sem_t *nfc_jni_llcp_sem;
static sem_t *nfc_jni_open_sem;
static sem_t *nfc_jni_init_sem;
static sem_t *nfc_jni_ioctl_sem;

static NFCSTATUS            nfc_jni_cb_status = NFCSTATUS_FAILED;

static jmethodID cached_NfcManager_notifyNdefMessageListeners;
static jmethodID cached_NfcManager_notifyTransactionListeners;
static jmethodID cached_NfcManager_notifyLlcpLinkActivation;
static jmethodID cached_NfcManager_notifyLlcpLinkDeactivated;
static jmethodID cached_NfcManager_notifyTargetDeselected;

namespace android {

phLibNfc_Handle     hIncommingLlcpSocket;
phLibNfc_Handle     storedHandle = 0;
sem_t               *nfc_jni_llcp_listen_sem;

struct nfc_jni_native_data *exported_nat = NULL;

/* TODO: move product specific configuration such as this
 * antenna tuning into product specific configuration */
uint8_t EEDATA_Settings[EEDATA_SETTINGS_NUMBER][4] = {
	// DIFFERENTIAL_ANTENNA

	// RF Settings
	{0x00,0x9B,0xD1,0x0D} // Tx consumption higher than 0x0D (average 50mA)
	,{0x00,0x9B,0xD2,0x24} // GSP setting for this threshold
	,{0x00,0x9B,0xD3,0x0A} // Tx consumption higher than 0x0A (average 40mA)
	,{0x00,0x9B,0xD4,0x22} // GSP setting for this threshold
	,{0x00,0x9B,0xD5,0x08} // Tx consumption higher than 0x08 (average 30mA)
	,{0x00,0x9B,0xD6,0x1E} // GSP setting for this threshold
	,{0x00,0x9B,0xDD,0x1C} // GSP setting for this threshold
	,{0x00,0x9B,0x84,0x13} // ANACM2 setting
	,{0x00,0x99,0x81,0x7F} // ANAVMID setting PCD
	,{0x00,0x99,0x31,0x70} // ANAVMID setting PICC

	// Enable PBTF
	,{0x00,0x98,0x00,0x3F} // SECURE_ELEMENT_CONFIGURATION - No Secure Element
	,{0x00,0x9F,0x09,0x00} // SWP_PBTF_RFU
	,{0x00,0x9F,0x0A,0x05} // SWP_PBTF_RFLD  --> RFLEVEL Detector for PBTF
	,{0x00,0x9E,0xD1,0xA1} //

	// Change RF Level Detector ANARFLDWU
	,{0x00,0x99,0x23,0x00} // Default Value is 0x01

	// Polling Loop Optimisation Detection  - 0x86 to enable - 0x00 to disable
	,{0x00,0x9E,0x74,0x00} // Default Value is 0x00, bits 0->2: sensitivity (0==maximal, 6==minimal), bits 3->6: RFU, bit 7: (0 -> disabled, 1 -> enabled)

	// Polling Loop - Card Emulation Timeout
	,{0x00,0x9F,0x35,0x14} // Time for which PN544 stays in Card Emulation mode after leaving RF field
	,{0x00,0x9F,0x36,0x60} // Default value 0x0411 = 50 ms ---> New Value : 0x1460 = 250 ms

	//LLC Timer
	,{0x00,0x9C,0x31,0x00} //
	,{0x00,0x9C,0x32,0x00} // 
	,{0x00,0x9C,0x0C,0x00} //
	,{0x00,0x9C,0x0D,0x00} //
};

/* Internal functions declaration */
static void *nfc_jni_client_thread(void *arg);
static void nfc_jni_init_callback(void *pContext, NFCSTATUS status);
static void nfc_jni_deinit_callback(void *pContext, NFCSTATUS status);
static void nfc_jni_discover_callback(void *pContext, NFCSTATUS status);
static void nfc_jni_se_set_mode_callback(void *context,
   phLibNfc_Handle handle, NFCSTATUS status);
static void nfc_jni_start_discovery_locked(struct nfc_jni_native_data *nat);

static phLibNfc_eConfigLinkType parseLinkType(const char* link_name)
{
   struct link_name_entry {
      phLibNfc_eConfigLinkType   value;
      const char *               name;
   };
   const struct link_name_entry sLinkNameTable[] = {
      {ENUM_LINK_TYPE_COM1, "COM1"},
      {ENUM_LINK_TYPE_COM2, "COM2"},
      {ENUM_LINK_TYPE_COM3, "COM3"},
      {ENUM_LINK_TYPE_COM4, "COM4"},
      {ENUM_LINK_TYPE_COM5, "COM5"},
      {ENUM_LINK_TYPE_COM6, "COM6"},
      {ENUM_LINK_TYPE_COM7, "COM7"},
      {ENUM_LINK_TYPE_COM8, "COM8"},
      {ENUM_LINK_TYPE_I2C,  "I2C"},
      {ENUM_LINK_TYPE_USB,  "USB"},
   };
   phLibNfc_eConfigLinkType ret;
   unsigned int i;

   /* NOTE: ENUM_LINK_TYPE_NB corresponds to undefined link name  */

   if (link_name == NULL)
   {
      return ENUM_LINK_TYPE_NB;
   }

   ret = ENUM_LINK_TYPE_NB;
   for (i=0 ; i<sizeof(sLinkNameTable)/sizeof(link_name_entry) ; i++)
   {
      if (strcmp(sLinkNameTable[i].name, link_name) == 0)
      {
         ret = sLinkNameTable[i].value;
         break;
      }
   }

   return ret;
}


/*
 * Deferred callback called when client thread must be exited
 */
static void client_kill_deferred_call(void* arg)
{
   struct nfc_jni_native_data *nat = (struct nfc_jni_native_data *)arg;
   
   nat->running = FALSE;
}

static void kill_client(nfc_jni_native_data *nat)
{
   phDal4Nfc_Message_Wrapper_t  wrapper;
   phLibNfc_DeferredCall_t     *pMsg;
   
   LOGD("Terminating client thead...");
    
   pMsg = (phLibNfc_DeferredCall_t*)malloc(sizeof(phLibNfc_DeferredCall_t));
   pMsg->pCallback = client_kill_deferred_call;
   pMsg->pParameter = (void*)nat;
   
   wrapper.msg.eMsgType = PH_LIBNFC_DEFERREDCALL_MSG;
   wrapper.msg.pMsgData = pMsg;
   wrapper.msg.Size     = sizeof(phLibNfc_DeferredCall_t);

   phDal4Nfc_msgsnd(gDrvCfg.nClientId, (struct msgbuf *)&wrapper, sizeof(phLibNfc_Message_t), 0);
}

static void nfc_jni_ioctl_callback(void *pContext, phNfc_sData_t *pOutput, NFCSTATUS status)
{
   LOG_CALLBACK("nfc_jni_ioctl_callback", status);

   nfc_jni_cb_status = status;

   sem_post(nfc_jni_ioctl_sem);
}



/* Initialization function */
static int nfc_jni_initialize(struct nfc_jni_native_data *nat)
{
   struct timespec ts;
   uint8_t resp[16];
   NFCSTATUS status;
   phLibNfc_StackCapabilities_t caps;
   char value[PROPERTY_VALUE_MAX];
   int result = FALSE;
   phLibNfc_SE_List_t SE_List[PHLIBNFC_MAXNO_OF_SE];
   uint8_t i, No_SE = PHLIBNFC_MAXNO_OF_SE, SmartMX_index=0, SmartMX_detected = 0;
   
   LOGD("Start Initialization\n");

   /* Reset stored handle */
   storedHandle = 0;

   /* Configure hardware link */
   gDrvCfg.nClientId = phDal4Nfc_msgget(0, 0600);
   
   property_get("ro.nfc.port", value, "unknown");
   gDrvCfg.nLinkType = parseLinkType(value);

   TRACE("phLibNfc_Mgt_ConfigureDriver(0x%08x, 0x%08x)", gDrvCfg.nClientId, gDrvCfg.nLinkType);
   REENTRANCE_LOCK();
   status = phLibNfc_Mgt_ConfigureDriver(&gDrvCfg, &gHWRef);
   REENTRANCE_UNLOCK();
   if(status == NFCSTATUS_ALREADY_INITIALISED)
   {
      LOGW("phLibNfc_Mgt_ConfigureDriver() returned 0x%04x[%s]", status, nfc_jni_get_status_name(status));
   }
   else if(status != NFCSTATUS_SUCCESS)
   {
      LOGE("phLibNfc_Mgt_ConfigureDriver() returned 0x%04x[%s]", status, nfc_jni_get_status_name(status));
      goto clean_and_return;
   }
   TRACE("phLibNfc_Mgt_ConfigureDriver() returned 0x%04x[%s]", status, nfc_jni_get_status_name(status));
   
   /* TODO: here would be a good place to perform HW reset of the chip */
   
   if(pthread_create(&(nat->thread), NULL, nfc_jni_client_thread,
         nat) != 0)
   {
      LOGE("pthread_create failed");
      goto clean_and_return;
   }
     
   TRACE("phLibNfc_Mgt_Initialize()");
   REENTRANCE_LOCK();
   status = phLibNfc_Mgt_Initialize(gHWRef, nfc_jni_init_callback, (void *)nat);
   REENTRANCE_UNLOCK();
   if(status != NFCSTATUS_PENDING)
   {
      LOGE("phLibNfc_Mgt_Initialize() returned 0x%04x[%s]", status, nfc_jni_get_status_name(status));
      goto clean_and_return;
   }
   TRACE("phLibNfc_Mgt_Initialize returned 0x%04x[%s]", status, nfc_jni_get_status_name(status));
  
   /* Wait for callback response */
   sem_wait(nfc_jni_init_sem);

   /* Initialization Status */
   if(nat->status != NFCSTATUS_SUCCESS)
   {
      goto clean_and_return;
   }

   // Update EEPROM settings
   TRACE("******  START EEPROM SETTINGS UPDATE ******");
   for(i=0; i< EEDATA_SETTINGS_NUMBER; i++)
   {
	   gInputParam.buffer  = EEDATA_Settings[i];
	   gInputParam.length  = 0x04;
	   gOutputParam.buffer = resp;

	   TRACE("> EEPROM SETTING: %d",i);
	   REENTRANCE_LOCK();
	   status = phLibNfc_Mgt_IoCtl(gHWRef,NFC_MEM_WRITE, &gInputParam, &gOutputParam, nfc_jni_ioctl_callback, (void *)nat);
	   REENTRANCE_UNLOCK();
	   if(status != NFCSTATUS_PENDING)
	   {
	      LOGE("phLibNfc_Mgt_IoCtl() returned 0x%04x[%s]", status, nfc_jni_get_status_name(status));
	      goto clean_and_return;
	   }
	   /* Wait for callback response */
	   sem_wait(nfc_jni_ioctl_sem);

	   /* Initialization Status */
	   if(nfc_jni_cb_status != NFCSTATUS_SUCCESS)
	   {
	      goto clean_and_return;
	   }
   }
   TRACE("******  ALL EEPROM SETTINGS UPDATED  ******");


   REENTRANCE_LOCK();
   status = phLibNfc_Mgt_GetstackCapabilities(&caps, (void *)nat);
   REENTRANCE_UNLOCK();
   if(status != NFCSTATUS_SUCCESS)
   {
      LOGW("phLibNfc_Mgt_GetstackCapabilities returned 0x%04x[%s]", status, nfc_jni_get_status_name(status));
   }
   else
   {
      LOGD("NFC capabilities: HAL = %x, FW = %x, HW = %x, Model = %x, HCI = %x",
         caps.psDevCapabilities.hal_version,
         caps.psDevCapabilities.fw_version,
         caps.psDevCapabilities.hw_version,
         caps.psDevCapabilities.model_id,
         caps.psDevCapabilities.hci_version);
   }

      
      /* Get Secure Element List */
      REENTRANCE_LOCK();
      TRACE("phLibNfc_SE_GetSecureElementList()");
      status = phLibNfc_SE_GetSecureElementList(SE_List, &No_SE);
      REENTRANCE_UNLOCK();
      if (status == NFCSTATUS_SUCCESS)
      {   
        TRACE("\n> Number of Secure Element(s) : %d\n", No_SE);
        /* Display Secure Element information */
        for (i = 0; i<No_SE; i++)
        {
          if (SE_List[i].eSE_Type == phLibNfc_SE_Type_SmartMX)
          {
            TRACE("phLibNfc_SE_GetSecureElementList(): SMX detected"); 
          }
          else if(SE_List[i].eSE_Type == phLibNfc_SE_Type_UICC)
          {
            TRACE("phLibNfc_SE_GetSecureElementList(): UICC detected"); 
          }
          
          /* Set SE mode - Off */
          TRACE("******  Initialize Secure Element ******");
          REENTRANCE_LOCK();
          status = phLibNfc_SE_SetMode(SE_List[i].hSecureElement,phLibNfc_SE_ActModeOff, nfc_jni_se_set_mode_callback,(void *)nat);
          REENTRANCE_UNLOCK();
       
          LOGD("phLibNfc_SE_SetMode for SE 0x%02x returned 0x%02x",SE_List[i].hSecureElement, status);
          if(status != NFCSTATUS_PENDING)
          {
            LOGE("phLibNfc_SE_SetMode() returned 0x%04x[%s]", status, nfc_jni_get_status_name(status));
            goto clean_and_return;
          }
          TRACE("phLibNfc_SE_SetMode() returned 0x%04x[%s]", status, nfc_jni_get_status_name(status));

          /* Wait for callback response */
          sem_wait(nfc_jni_manager_sem);
        }
      }
      else
      {
        LOGD("phLibNfc_SE_GetSecureElementList(): Error");
      }
      
   LOGI("NFC Initialized");

   result = TRUE;

clean_and_return:
   if (result != TRUE)
   {
      if(nat)
      {
         kill_client(nat);
      }
   }
   
   return result;
}


/*
 * Last-chance fallback when there is no clean way to recover
 * Performs a software reset
  */
void emergency_recovery(struct nfc_jni_native_data *nat)
{
   phLibNfc_sADD_Cfg_t discovery_cfg;
   phLibNfc_Registry_Info_t registration_cfg;
   
   LOGE("emergency_recovery: force restart of NFC service");
   abort();  // force a noisy crash
}

/*
 * Restart the polling loop when unable to perform disconnect
  */
void nfc_jni_restart_discovery_locked(struct nfc_jni_native_data *nat)
{
   int ret;

   TRACE("Restarting polling loop");
   
   /* Restart Polling loop */
   TRACE("******  Start NFC Discovery ******");
   REENTRANCE_LOCK();
   ret = phLibNfc_Mgt_ConfigureDiscovery(NFC_DISCOVERY_RESUME,nat->discovery_cfg, nfc_jni_discover_callback, (void *)nat);
   REENTRANCE_UNLOCK();
   TRACE("phLibNfc_Mgt_ConfigureDiscovery(%s-%s-%s-%s-%s-%s, %s-%x-%x) returned 0x%08x\n",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443A==TRUE?"3A":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443B==TRUE?"3B":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica212==TRUE?"F2":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica424==TRUE?"F4":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableNfcActive==TRUE?"NFC":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso15693==TRUE?"RFID":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.DisableCardEmulation==FALSE?"CE":"",
      nat->discovery_cfg.NfcIP_Mode, nat->discovery_cfg.Duration, ret);
   
   if (ret != NFCSTATUS_PENDING)
   {
        emergency_recovery(nat);
   }
   else
   {
        /* Wait for callback response */
        sem_wait(nfc_jni_manager_sem);
        if(nfc_jni_cb_status != NFCSTATUS_SUCCESS)
        {
                LOGD("phLibNfc_Mgt_ConfigureDiscovery callback returned 0x%08x ", nfc_jni_cb_status);
                return;
        }
        TRACE("phLibNfc_Mgt_ConfigureDiscovery callback returned 0x%08x ", nfc_jni_cb_status);;
   }     
}

/*
 *  Utility to get target type name from its specs
 */
static const char* get_target_type_name(phNfc_eRemDevType_t type, uint8_t sak)
{
   switch (type)
   {
      case phNfc_eISO14443_A_PICC:
      case phNfc_eISO14443_4A_PICC:
      case phNfc_eISO14443_4B_PICC:
        {
          return TARGET_TYPE_ISO14443_4;
        }break;
        
      case phNfc_eISO14443_3A_PICC:
        {
          return TARGET_TYPE_ISO14443_3A;
        }break;

      case phNfc_eISO14443_B_PICC:
        {
          /* Actually this can be -3B or -4B
           * FRI doesn't allow us to tell the diff yet
           * and the API doesn't know type 4B
           * so return 3B for now.
           */
          return TARGET_TYPE_ISO14443_3B;
        }break;
        
      case phNfc_eISO15693_PICC:
        {
          return TARGET_TYPE_ISO15693;
        }break;

      case phNfc_eMifare_PICC:
        {
          switch(sak)
          {
            case 0:
              {
                return TARGET_TYPE_MIFARE_UL;
              }break;
            case 8:
              {
                return TARGET_TYPE_MIFARE_1K;
              }break;
            case 24:
              {
                return TARGET_TYPE_MIFARE_4K;
              }break;
              
            case 32:
              {
                return TARGET_TYPE_MIFARE_DESFIRE;
              }break;
              
            default:
              {
                return TARGET_TYPE_MIFARE_UNKNOWN;
              }break;
          }
        }break;
      case phNfc_eFelica_PICC:
        {
          return TARGET_TYPE_FELICA;
        }break; 
      case phNfc_eJewel_PICC:
        {
          return TARGET_TYPE_JEWEL;
        }break; 
      default:
        {
          return TARGET_TYPE_UNKNOWN;
        }
   }

   return TARGET_TYPE_UNKNOWN;
}

 /*
+ *  Utility to recover UID from target infos
+ */
static phNfc_sData_t get_target_uid(phLibNfc_sRemoteDevInformation_t *psRemoteDevInfo)
{
    phNfc_sData_t uid;

    switch(psRemoteDevInfo->RemDevType)
    {
    case phNfc_eISO14443_A_PICC:
    case phNfc_eISO14443_4A_PICC:
    case phNfc_eISO14443_3A_PICC:
    case phNfc_eMifare_PICC:
        uid.buffer = psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.Uid;
        uid.length = psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.UidLength;
        break;
    case phNfc_eISO14443_B_PICC:
    case phNfc_eISO14443_4B_PICC:
        uid.buffer = psRemoteDevInfo->RemoteDevInfo.Iso14443B_Info.AtqB.AtqResInfo.Pupi;
        uid.length = sizeof(psRemoteDevInfo->RemoteDevInfo.Iso14443B_Info.AtqB.AtqResInfo.Pupi);
        break;
    case phNfc_eFelica_PICC:
        uid.buffer = psRemoteDevInfo->RemoteDevInfo.Felica_Info.IDm;
        uid.length = psRemoteDevInfo->RemoteDevInfo.Felica_Info.IDmLength;
        break;
    case phNfc_eJewel_PICC:
        uid.buffer = psRemoteDevInfo->RemoteDevInfo.Jewel_Info.Uid;
        uid.length = psRemoteDevInfo->RemoteDevInfo.Jewel_Info.UidLength;
        break;
    case phNfc_eISO15693_PICC:
        uid.buffer = psRemoteDevInfo->RemoteDevInfo.Iso15693_Info.Uid;
        uid.length = psRemoteDevInfo->RemoteDevInfo.Iso15693_Info.UidLength;
        break;
    case phNfc_eNfcIP1_Target:
    case phNfc_eNfcIP1_Initiator:
        uid.buffer = psRemoteDevInfo->RemoteDevInfo.NfcIP_Info.NFCID;
        uid.length = psRemoteDevInfo->RemoteDevInfo.NfcIP_Info.NFCID_Length;
        break;
    default:
        uid.buffer = NULL;
        uid.length = 0;
        break;
    }

    return uid;
}
/*
 *  Utility to recover poll bytes from target infos
 */
static void set_target_pollBytes(JNIEnv *e, struct nfc_jni_native_data *nat, jobject tag, phLibNfc_sRemoteDevInformation_t *psRemoteDevInfo)
{
    jclass tag_cls = e->GetObjectClass(nat->cached_NfcTag);
    jfieldID f;
    jbyteArray tagBytes;

    f = e->GetFieldID(tag_cls, "mPollBytes", "[B");
    TRACE("psRemoteDevInfo->RemDevType %x", psRemoteDevInfo->RemDevType);
    switch(psRemoteDevInfo->RemDevType)
    {
        /* ISO14443-3A: ATQA/SENS_RES */
        case phNfc_eISO14443_A_PICC:
        case phNfc_eISO14443_3A_PICC:
        case phNfc_eISO14443_4A_PICC:
        case phNfc_eMifare_PICC:
            tagBytes = e->NewByteArray(sizeof(psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.AtqA));
            e->SetByteArrayRegion(tagBytes, 0, sizeof(psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.AtqA),
                                  (jbyte *)psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.AtqA);
            break;
        /* ISO14443-3B: Application data (4 bytes) and Protocol Info (3 bytes) from ATQB/SENSB_RES */
        case phNfc_eISO14443_B_PICC:
        case phNfc_eISO14443_4B_PICC:
            tagBytes = e->NewByteArray(sizeof(psRemoteDevInfo->RemoteDevInfo.Iso14443B_Info.AtqB.AtqResInfo.AppData) 
                                       + sizeof(psRemoteDevInfo->RemoteDevInfo.Iso14443B_Info.AtqB.AtqResInfo.ProtInfo));
            e->SetByteArrayRegion(tagBytes, 0, sizeof(psRemoteDevInfo->RemoteDevInfo.Iso14443B_Info.AtqB.AtqResInfo.AppData),
                                  (jbyte *)psRemoteDevInfo->RemoteDevInfo.Iso14443B_Info.AtqB.AtqResInfo.AppData);
            e->SetByteArrayRegion(tagBytes, sizeof(psRemoteDevInfo->RemoteDevInfo.Iso14443B_Info.AtqB.AtqResInfo.AppData),
                                  sizeof(psRemoteDevInfo->RemoteDevInfo.Iso14443B_Info.AtqB.AtqResInfo.ProtInfo),
                                  (jbyte *)psRemoteDevInfo->RemoteDevInfo.Iso14443B_Info.AtqB.AtqResInfo.ProtInfo);
            break;
        /* JIS_X_6319_4: PAD0 (2 byte), PAD1 (2 byte), MRTI(2 byte), PAD2 (1 byte), RC (2 byte) */
        case phNfc_eFelica_PICC:
            tagBytes = e->NewByteArray(sizeof(psRemoteDevInfo->RemoteDevInfo.Felica_Info.PMm)
                                       + sizeof(psRemoteDevInfo->RemoteDevInfo.Felica_Info.SystemCode));
            e->SetByteArrayRegion(tagBytes, 0, sizeof(psRemoteDevInfo->RemoteDevInfo.Felica_Info.PMm),
                                  (jbyte *)psRemoteDevInfo->RemoteDevInfo.Felica_Info.PMm);
            e->SetByteArrayRegion(tagBytes, sizeof(psRemoteDevInfo->RemoteDevInfo.Felica_Info.PMm),
                                  sizeof(psRemoteDevInfo->RemoteDevInfo.Felica_Info.SystemCode),
                                  (jbyte *)psRemoteDevInfo->RemoteDevInfo.Felica_Info.SystemCode);
            break;
        /* ISO15693: response flags (1 byte), DSFID (1 byte) */
        case phNfc_eISO15693_PICC:
            tagBytes = e->NewByteArray(sizeof(psRemoteDevInfo->RemoteDevInfo.Iso15693_Info.Flags)
                                       + sizeof(psRemoteDevInfo->RemoteDevInfo.Iso15693_Info.Dsfid));
            e->SetByteArrayRegion(tagBytes, 0, sizeof(psRemoteDevInfo->RemoteDevInfo.Iso15693_Info.Flags),
                                  (jbyte *)&psRemoteDevInfo->RemoteDevInfo.Iso15693_Info.Flags);
            e->SetByteArrayRegion(tagBytes, sizeof(psRemoteDevInfo->RemoteDevInfo.Iso15693_Info.Flags),
                                  sizeof(psRemoteDevInfo->RemoteDevInfo.Iso15693_Info.Dsfid),
                                  (jbyte *)&psRemoteDevInfo->RemoteDevInfo.Iso15693_Info.Dsfid);
            break;
        default:
            tagBytes = e->NewByteArray(0);
            break;
    }

    e->SetObjectField(tag, f, tagBytes);
}

/*
 *  Utility to recover activation bytes from target infos
 */
static void set_target_activationBytes(JNIEnv *e, struct nfc_jni_native_data *nat, jobject tag, phLibNfc_sRemoteDevInformation_t *psRemoteDevInfo)
{
    jclass tag_cls = e->GetObjectClass(nat->cached_NfcTag);
    jfieldID f;
    jbyteArray tagBytes;

    f = e->GetFieldID(tag_cls, "mActivationBytes", "[B");
    switch(psRemoteDevInfo->RemDevType)
    {
        /* ISO14443-3A: SAK/SEL_RES */
        case phNfc_eISO14443_3A_PICC:
        case phNfc_eMifare_PICC:
            tagBytes = e->NewByteArray(sizeof(psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.Sak));
            e->SetByteArrayRegion(tagBytes, 0, sizeof(psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.Sak),
                                  (jbyte *)&psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.Sak);
            break;
        /* ISO14443-3A & ISO14443-4: SAK/SEL_RES, historical bytes from ATS */
        case phNfc_eISO14443_A_PICC:
        case phNfc_eISO14443_4A_PICC:
            tagBytes = e->NewByteArray(sizeof(psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.Sak)
                                       + psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.AppDataLength);
            e->SetByteArrayRegion(tagBytes, 0, sizeof(psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.Sak),
                                  (jbyte *)&psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.Sak);
            e->SetByteArrayRegion(tagBytes, sizeof(psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.Sak),
                                  psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.AppDataLength,
                                  (jbyte *)psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.AppData);
            break;
        /* ISO14443-3B & ISO14443-4: ATTRIB response */
        case phNfc_eISO14443_B_PICC:
        case phNfc_eISO14443_4B_PICC:
            tagBytes = e->NewByteArray(psRemoteDevInfo->RemoteDevInfo.Iso14443B_Info.HiLayerRespLength);
            e->SetByteArrayRegion(tagBytes, 0, psRemoteDevInfo->RemoteDevInfo.Iso14443B_Info.HiLayerRespLength,
                                  (jbyte *)psRemoteDevInfo->RemoteDevInfo.Iso14443B_Info.HiLayerResp);
            break;
        /* ISO15693: response flags (1 byte), DSFID (1 byte) */
        case phNfc_eISO15693_PICC:
            tagBytes = e->NewByteArray(sizeof(psRemoteDevInfo->RemoteDevInfo.Iso15693_Info.Flags)
                                       + sizeof(psRemoteDevInfo->RemoteDevInfo.Iso15693_Info.Dsfid));
            e->SetByteArrayRegion(tagBytes, 0, sizeof(psRemoteDevInfo->RemoteDevInfo.Iso15693_Info.Flags),
                                  (jbyte *)&psRemoteDevInfo->RemoteDevInfo.Iso15693_Info.Flags);
            e->SetByteArrayRegion(tagBytes, sizeof(psRemoteDevInfo->RemoteDevInfo.Iso15693_Info.Flags),
                                  sizeof(psRemoteDevInfo->RemoteDevInfo.Iso15693_Info.Dsfid),
                                  (jbyte *)&psRemoteDevInfo->RemoteDevInfo.Iso15693_Info.Dsfid);
            break;
        default:
            tagBytes = e->NewByteArray(0);
            break;
    }

    e->SetObjectField(tag, f, tagBytes);
}

/*
 * NFC stack message processing
 */
static void *nfc_jni_client_thread(void *arg)
{
   struct nfc_jni_native_data *nat;
   JNIEnv *e;
   JavaVMAttachArgs thread_args;
   phDal4Nfc_Message_Wrapper_t wrapper;

   nat = (struct nfc_jni_native_data *)arg;

   thread_args.name = "NFC Message Loop";
   thread_args.version = nat->env_version;
   thread_args.group = NULL;

   nat->vm->AttachCurrentThread(&e, &thread_args);
   pthread_setname_np(pthread_self(), "message");

   TRACE("NFC client started");
   nat->running = TRUE;
   while(nat->running == TRUE)
   {
      /* Fetch next message from the NFC stack message queue */
      if(phDal4Nfc_msgrcv(gDrvCfg.nClientId, (void *)&wrapper,
         sizeof(phLibNfc_Message_t), 0, 0) == -1)
      {
         LOGE("NFC client received bad message");
         continue;
      }

      switch(wrapper.msg.eMsgType)
      {
         case PH_LIBNFC_DEFERREDCALL_MSG:
         {
            phLibNfc_DeferredCall_t *msg =
               (phLibNfc_DeferredCall_t *)(wrapper.msg.pMsgData);

            REENTRANCE_LOCK();
            msg->pCallback(msg->pParameter);
            REENTRANCE_UNLOCK();

            break;
         }
      }
   }
   TRACE("NFC client stopped");
   
   nat->vm->DetachCurrentThread();

   return NULL;
}

extern uint8_t nfc_jni_is_ndef;
extern uint8_t *nfc_jni_ndef_buf;
extern uint32_t nfc_jni_ndef_buf_len;

static phLibNfc_sNfcIPCfg_t nfc_jni_nfcip1_cfg =
{
   3,
   { 0x46, 0x66, 0x6D }
}; 

/*
 * Callbacks
 */

/* P2P - LLCP callbacks */
static void nfc_jni_llcp_linkStatus_callback(void *pContext,
                                                    phFriNfc_LlcpMac_eLinkStatus_t   eLinkStatus)
{
   phFriNfc_Llcp_sLinkParameters_t  sLinkParams;
   JNIEnv *e;
   NFCSTATUS status;
   struct nfc_jni_native_data *nat;

   nat = (struct nfc_jni_native_data *)pContext;
   
   TRACE("Callback: nfc_jni_llcp_linkStatus_callback()");

   nat->vm->GetEnv( (void **)&e, nat->env_version);
   
   /* Update link status */
   g_eLinkStatus = eLinkStatus;

   if(eLinkStatus == phFriNfc_LlcpMac_eLinkActivated)
   {
      REENTRANCE_LOCK();
      status = phLibNfc_Llcp_GetRemoteInfo(hLlcpHandle, &sLinkParams);
      REENTRANCE_UNLOCK();
      if(status != NFCSTATUS_SUCCESS)
      {
           LOGW("GetRemote Info failded - Status = %02x",status);
      }
      else
      {
           LOGI("LLCP Link activated (LTO=%d, MIU=%d, OPTION=0x%02x, WKS=0x%02x)",sLinkParams.lto,
                                                                                  sLinkParams.miu,
                                                                                  sLinkParams.option,
                                                                                  sLinkParams.wks);
      }
   }
   else if(eLinkStatus == phFriNfc_LlcpMac_eLinkDeactivated)
   {
      LOGI("LLCP Link deactivated");
      /* Notify manager that the LLCP is lost or deactivated */
      e->CallVoidMethod(nat->manager, cached_NfcManager_notifyLlcpLinkDeactivated, nat->tag);
      if(e->ExceptionCheck())
      {
         LOGE("Exception occured");
         kill_client(nat);
      } 
   }
}

static void nfc_jni_checkLlcp_callback(void *context,
                                              NFCSTATUS status)
{
   nfc_jni_cb_status = status;
   
   PHNFC_UNUSED_VARIABLE(context);

   LOG_CALLBACK("nfc_jni_checkLlcp_callback", status);

   if(status == NFCSTATUS_SUCCESS)
   {
      TRACE("%s return status = 0x%x\n", __func__, status);

      sem_post(nfc_jni_llcp_sem);
   }
}

static void nfc_jni_llcpcfg_callback(void *pContext, NFCSTATUS status)
{
   nfc_jni_cb_status = status;
   
   LOG_CALLBACK("nfc_jni_llcpcfg_callback", status);

   sem_post(nfc_jni_manager_sem);
}

static void nfc_jni_p2pcfg_callback(void *pContext, NFCSTATUS status)
{
   nfc_jni_cb_status = status;

   LOG_CALLBACK("nfc_jni_p2pcfg_callback", status);

   sem_post(nfc_jni_manager_sem);
}

static void nfc_jni_llcp_transport_listen_socket_callback(void              *pContext,
                                                                 phLibNfc_Handle   IncomingSocket)
{
   PHNFC_UNUSED_VARIABLE(pContext);

   TRACE("Callback: nfc_jni_llcp_transport_listen_socket_callback()");

   if(IncomingSocket != 0)
   {
      TRACE("Listen CB \n");
      hIncommingLlcpSocket = IncomingSocket;
      sem_post(nfc_jni_llcp_listen_sem);
   }
   else
   {
      LOGW("Listen KO");
   }
}

void nfc_jni_llcp_transport_socket_err_callback(void*      pContext,
                                                       uint8_t    nErrCode)
{
   PHNFC_UNUSED_VARIABLE(pContext);

   TRACE("Callback: nfc_jni_llcp_transport_socket_err_callback()");

   if(nErrCode == PHFRINFC_LLCP_ERR_FRAME_REJECTED)
   {
      LOGW("Frame Rejected - Disconnected");
   }
   else if(nErrCode == PHFRINFC_LLCP_ERR_DISCONNECTED)
   {
      LOGD("Socket Disconnected");
   }
}

static void nfc_jni_connect_callback(void *pContext,
   phLibNfc_Handle hRemoteDev,
   phLibNfc_sRemoteDevInformation_t *psRemoteDevInfo, NFCSTATUS status)
{
   LOG_CALLBACK("nfc_jni_connect_callback", status);
}


static void nfc_jni_discover_callback(void *pContext, NFCSTATUS status)
{
   nfc_jni_cb_status = status;

   sem_post(nfc_jni_manager_sem);
}

static void nfc_jni_Discovery_notification_callback(void *pContext,
   phLibNfc_RemoteDevList_t *psRemoteDevList,
   uint8_t uNofRemoteDev, NFCSTATUS status)
{
   JNIEnv *e;
   NFCSTATUS ret;
   jclass tag_cls = NULL;
   jobject target_array;
   jobject tag;
   jmethodID ctor;
   jfieldID f;
   const char * typeName;
   jbyteArray tagUid;
   jbyteArray generalBytes = NULL;
   struct nfc_jni_native_data *nat;
   struct timespec ts;
   phNfc_sData_t data;
   int i;
   int target_index = 0; // Target that will be reported (if multiple can be >0)

   nat = (struct nfc_jni_native_data *)pContext;
   
   nat->vm->GetEnv( (void **)&e, nat->env_version);
   
   if(status == NFCSTATUS_DESELECTED)
   {
      LOG_CALLBACK("nfc_jni_Discovery_notification_callback: Target deselected", status); 
         
      /* Notify manager that a target was deselected */
      e->CallVoidMethod(nat->manager, cached_NfcManager_notifyTargetDeselected);
      if(e->ExceptionCheck())
      {
         LOGE("Exception occured");
         kill_client(nat);
      } 
   }
   else
   {
      LOG_CALLBACK("nfc_jni_Discovery_notification_callback", status);
      TRACE("Discovered %d tags", uNofRemoteDev);
      
      if((psRemoteDevList->psRemoteDevInfo->RemDevType == phNfc_eNfcIP1_Initiator)
          || (psRemoteDevList->psRemoteDevInfo->RemDevType == phNfc_eNfcIP1_Target))
      {
         tag_cls = e->GetObjectClass(nat->cached_P2pDevice);
         if(e->ExceptionCheck())
         {
            LOGE("Get Object Class Error"); 
            kill_client(nat);
            return;
         } 
         
         /* New target instance */
         ctor = e->GetMethodID(tag_cls, "<init>", "()V");
         tag = e->NewObject(tag_cls, ctor);
         
         /* Set P2P Target mode */
         f = e->GetFieldID(tag_cls, "mMode", "I"); 
         
         if(psRemoteDevList->psRemoteDevInfo->RemDevType == phNfc_eNfcIP1_Initiator)
         {
            LOGD("Discovered P2P Initiator");
            e->SetIntField(tag, f, (jint)MODE_P2P_INITIATOR);
         }
         else
         {    
            LOGD("Discovered P2P Target");
            e->SetIntField(tag, f, (jint)MODE_P2P_TARGET);
         }
          
         if(psRemoteDevList->psRemoteDevInfo->RemDevType == phNfc_eNfcIP1_Initiator)
         {
            /* Set General Bytes */
            f = e->GetFieldID(tag_cls, "mGeneralBytes", "[B");
   
           TRACE("General Bytes length =");
           for(i=0;i<psRemoteDevList->psRemoteDevInfo->RemoteDevInfo.NfcIP_Info.ATRInfo_Length;i++)
           {
               LOGD("%02x ", psRemoteDevList->psRemoteDevInfo->RemoteDevInfo.NfcIP_Info.ATRInfo[i]);          
           }
       
            generalBytes = e->NewByteArray(psRemoteDevList->psRemoteDevInfo->RemoteDevInfo.NfcIP_Info.ATRInfo_Length);   
             
            e->SetByteArrayRegion(generalBytes, 0, 
                                  psRemoteDevList->psRemoteDevInfo->RemoteDevInfo.NfcIP_Info.ATRInfo_Length, 
                                  (jbyte *)psRemoteDevList->psRemoteDevInfo->RemoteDevInfo.NfcIP_Info.ATRInfo);
             
            e->SetObjectField(tag, f, generalBytes);
         }
      }
      else
      {
        tag_cls = e->GetObjectClass(nat->cached_NfcTag);
        if(e->ExceptionCheck())
        {
            kill_client(nat);
            return;
        }

        /* New tag instance */
        ctor = e->GetMethodID(tag_cls, "<init>", "()V");
        tag = e->NewObject(tag_cls, ctor);

        if(status == NFCSTATUS_MULTIPLE_PROTOCOLS)
        {
            TRACE("Multiple Protocol TAG detected\n");
            /* Since on most multi proto cards Mifare (emu) is first, and ISO second,
             * we prefer the second standard above Mifare. Proper fix is to parse it.
             */
            target_index = 1;
        }
        else
        {
            TRACE("Simple Protocol TAG detected\n");
            target_index = 0;
        }
        /* Set tag UID */
        f = e->GetFieldID(tag_cls, "mUid", "[B");
        data = get_target_uid(psRemoteDevList[target_index].psRemoteDevInfo);
        tagUid = e->NewByteArray(data.length);
        if(data.length > 0)
        {
            e->SetByteArrayRegion(tagUid, 0, data.length, (jbyte *)data.buffer);
        }
        e->SetObjectField(tag, f, tagUid);

        /* Set tag type */
        typeName = get_target_type_name( psRemoteDevList[target_index].psRemoteDevInfo->RemDevType,
                          psRemoteDevList[target_index].psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.Sak);
        LOGD("Discovered tag: type=0x%08x[%s]", psRemoteDevList[target_index].psRemoteDevInfo->RemDevType, typeName);
        f = e->GetFieldID(tag_cls, "mType", "Ljava/lang/String;");
        e->SetObjectField(tag, f, e->NewStringUTF(typeName));

      }
      /* Set tag handle */
      f = e->GetFieldID(tag_cls, "mHandle", "I");
      e->SetIntField(tag, f,(jint)psRemoteDevList[target_index].hTargetDev);
      TRACE("Target handle = 0x%08x",psRemoteDevList[target_index].hTargetDev);
      storedHandle = psRemoteDevList[target_index].hTargetDev;
      if (nat->tag != NULL) {
          e->DeleteGlobalRef(nat->tag);
      }
      nat->tag = e->NewGlobalRef(tag);

      /* Set tag polling bytes */
      TRACE("Set Tag PollBytes");
      set_target_pollBytes(e, nat, tag, psRemoteDevList[target_index].psRemoteDevInfo);

      /* Set tag activation bytes */
      TRACE("Set Tag ActivationBytes\n");
      set_target_activationBytes(e, nat, tag, psRemoteDevList[target_index].psRemoteDevInfo);
   
      /* Notify the service */   
      TRACE("Notify Nfc Service");
      if((psRemoteDevList->psRemoteDevInfo->RemDevType == phNfc_eNfcIP1_Initiator)
          || (psRemoteDevList->psRemoteDevInfo->RemDevType == phNfc_eNfcIP1_Target))
      {
         /* Store the hanlde of the P2P device */
         hLlcpHandle = psRemoteDevList->hTargetDev;
         
         /* Notify manager that new a P2P device was found */
         e->CallVoidMethod(nat->manager, cached_NfcManager_notifyLlcpLinkActivation, tag);
         if(e->ExceptionCheck())
         {
            LOGE("Exception occured");
            kill_client(nat);
         }     
      }
      else
      {
         /* Notify manager that new a tag was found */
         e->CallVoidMethod(nat->manager, cached_NfcManager_notifyNdefMessageListeners, tag);
         if(e->ExceptionCheck())
         {
            LOGE("Exception occured");
            kill_client(nat);
         }     
      }
      e->DeleteLocalRef(tag);
   } 
}

static void nfc_jni_init_callback(void *pContext, NFCSTATUS status)
{
   LOG_CALLBACK("nfc_jni_init_callback", status);

   struct nfc_jni_native_data *nat;

   nat = (struct nfc_jni_native_data *)pContext;

   nat->status = status;

   sem_post(nfc_jni_init_sem);
}

static void nfc_jni_deinit_callback(void *pContext, NFCSTATUS status)
{
   struct nfc_jni_native_data *nat =
      (struct nfc_jni_native_data *)pContext;

   LOG_CALLBACK("nfc_jni_deinit_callback", status);

   nat->status = status;
   kill_client(nat);

   sem_post(nfc_jni_manager_sem);
}

/* Set Secure Element mode callback*/
static void nfc_jni_smartMX_setModeCb (void*            pContext,
							                                phLibNfc_Handle  hSecureElement,
                                              NFCSTATUS        status)
{

  struct nfc_jni_native_data *nat =
    (struct nfc_jni_native_data *)pContext;
      
	if(status==NFCSTATUS_SUCCESS)
	{
		LOGD("SE Set Mode is Successful, handle: %u", hSecureElement);
	}
	else
	{
    LOGD("SE Set Mode is failed\n ");
  }
	
	nat->status = status;
	sem_post(nfc_jni_open_sem);
}

/* Card Emulation callback */
static void nfc_jni_transaction_callback(void *context,
   phLibNfc_eSE_EvtType_t evt_type, phLibNfc_Handle handle,
   phLibNfc_uSeEvtInfo_t *evt_info, NFCSTATUS status)
{
   JNIEnv *e;
   jobject aid_array;
   struct nfc_jni_native_data *nat;
   phNfc_sData_t *aid;

   LOG_CALLBACK("nfc_jni_transaction_callback", status);

   nat = (struct nfc_jni_native_data *)context;

   nat->vm->GetEnv( (void **)&e, nat->env_version);

   aid = &(evt_info->UiccEvtInfo.aid);

   aid_array = NULL;

   if(aid != NULL)
   {
      aid_array = e->NewByteArray(aid->length);
      if(e->ExceptionCheck())
      {
         LOGE("Exception occured");
         kill_client(nat);
         return;
      }

      e->SetByteArrayRegion((jbyteArray)aid_array, 0, aid->length, (jbyte *)aid->buffer);
   }
   
   TRACE("Notify Nfc Service\n");
   /* Notify manager that a new event occurred on a SE */
   e->CallVoidMethod(nat->manager,
      cached_NfcManager_notifyTransactionListeners, aid_array);
      
   if(e->ExceptionCheck())
   {
      LOGE("Notification Exception occured");
      kill_client(nat);
   }

   e->DeleteLocalRef(aid_array);
}

static void nfc_jni_se_set_mode_callback(void *context,
   phLibNfc_Handle handle, NFCSTATUS status)
{
   LOG_CALLBACK("nfc_jni_se_set_mode_callback", status);

   sem_post(nfc_jni_manager_sem);
}

/*
 * NFCManager methods
 */
 
static void nfc_jni_start_card_emu_discovery_locked(struct nfc_jni_native_data *nat)
{
   NFCSTATUS ret;
   
   TRACE("******  NFC Config Mode Card Emulation ******");   

   /* Register for the card emulation mode */
   REENTRANCE_LOCK();
   ret = phLibNfc_SE_NtfRegister(nfc_jni_transaction_callback,(void *)nat);
   REENTRANCE_UNLOCK();
   TRACE("phLibNfc_SE_NtfRegister returned 0x%x\n", ret);
   if(ret != NFCSTATUS_SUCCESS)
       return;
}


static void nfc_jni_start_discovery_locked(struct nfc_jni_native_data *nat)
{
   NFCSTATUS ret;
   phLibNfc_Llcp_sLinkParameters_t LlcpConfigInfo;

   /* Clear previous configuration */
   //memset(&nat->discovery_cfg, 0, sizeof(phLibNfc_sADD_Cfg_t));
   //memset(&nat->registry_info, 0, sizeof(phLibNfc_Registry_Info_t));

#if 0
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443A = TRUE;
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443B = TRUE;
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica212 = TRUE;
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica424 = TRUE;
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso15693  = TRUE;
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableNfcActive = FALSE;
#endif
   
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.DisableCardEmulation = FALSE;
   nat->discovery_cfg.NfcIP_Mode = phNfc_eDefaultP2PMode;
   nat->discovery_cfg.Duration = 300000; /* in ms */
   nat->discovery_cfg.NfcIP_Tgt_Disable = FALSE;


   nat->registry_info.MifareUL = TRUE;
   nat->registry_info.MifareStd = TRUE;
   nat->registry_info.ISO14443_4A = TRUE;
   nat->registry_info.ISO14443_4B = TRUE;
   nat->registry_info.Jewel = TRUE;
   nat->registry_info.Felica = TRUE;
   nat->registry_info.NFC = FALSE;
   nat->registry_info.ISO15693 = TRUE;
   TRACE("******  NFC Config Mode Reader ******");
      
   /* LLCP Params */
   TRACE("******  NFC Config Mode NFCIP1 - LLCP ******"); 
   LlcpConfigInfo.miu    = nat->miu;
   LlcpConfigInfo.lto    = nat->lto;
   LlcpConfigInfo.wks    = nat->wks;
   LlcpConfigInfo.option = nat->opt;
    
   REENTRANCE_LOCK();
   ret = phLibNfc_Mgt_SetLlcp_ConfigParams(&LlcpConfigInfo,
                                           nfc_jni_llcpcfg_callback,
                                           (void *)nat);
   REENTRANCE_UNLOCK();
   if(ret != NFCSTATUS_PENDING)
   {
        LOGD("phLibNfc_Mgt_SetLlcp_ConfigParams returned 0x%02x",ret);
        return;
   }
   TRACE("phLibNfc_Mgt_SetLlcp_ConfigParams returned 0x%02x",ret);

   /* Wait for callback response */
   sem_wait(nfc_jni_manager_sem);
   
   /* Register for the reader mode */
   REENTRANCE_LOCK();
   ret = phLibNfc_RemoteDev_NtfRegister(&nat->registry_info, nfc_jni_Discovery_notification_callback, (void *)nat);
   REENTRANCE_UNLOCK();
   if(ret != NFCSTATUS_SUCCESS)
   {
        LOGD("pphLibNfc_RemoteDev_NtfRegister returned 0x%02x",ret);
        return;
   }
   TRACE("phLibNfc_RemoteDev_NtfRegister(%s-%s-%s-%s-%s-%s-%s-%s) returned 0x%x\n",
      nat->registry_info.Jewel==TRUE?"J":"",
      nat->registry_info.MifareUL==TRUE?"UL":"",
      nat->registry_info.MifareStd==TRUE?"Mi":"",
      nat->registry_info.Felica==TRUE?"F":"",
      nat->registry_info.ISO14443_4A==TRUE?"4A":"",
      nat->registry_info.ISO14443_4B==TRUE?"4B":"",
      nat->registry_info.NFC==TRUE?"P2P":"",
      nat->registry_info.ISO15693==TRUE?"R":"", ret);

   
   /* Register for the card emulation mode */
   REENTRANCE_LOCK();
   ret = phLibNfc_SE_NtfRegister(nfc_jni_transaction_callback,(void *)nat);
   REENTRANCE_UNLOCK();
   if(ret != NFCSTATUS_SUCCESS)
   {
        LOGD("pphLibNfc_RemoteDev_NtfRegister returned 0x%02x",ret);
        return;
   }
   TRACE("phLibNfc_SE_NtfRegister returned 0x%x\n", ret);

   /* Start Polling loop */
   TRACE("******  Start NFC Discovery ******");
   REENTRANCE_LOCK();
   ret = phLibNfc_Mgt_ConfigureDiscovery(NFC_DISCOVERY_CONFIG,nat->discovery_cfg, nfc_jni_discover_callback, (void *)nat);
   REENTRANCE_UNLOCK();
   TRACE("phLibNfc_Mgt_ConfigureDiscovery(%s-%s-%s-%s-%s-%s, %s-%x-%x) returned 0x%08x\n",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443A==TRUE?"3A":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443B==TRUE?"3B":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica212==TRUE?"F2":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica424==TRUE?"F4":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableNfcActive==TRUE?"NFC":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso15693==TRUE?"RFID":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.DisableCardEmulation==FALSE?"CE":"",
      nat->discovery_cfg.NfcIP_Mode, nat->discovery_cfg.Duration, ret);

   if(ret != NFCSTATUS_PENDING)
   {
        emergency_recovery(nat);
   }

   /* Wait for callback response */
   sem_wait(nfc_jni_manager_sem);
   if(nfc_jni_cb_status != NFCSTATUS_SUCCESS)
   {
      LOGD("phLibNfc_Mgt_ConfigureDiscovery callback returned 0x%08x ", nfc_jni_cb_status);
      return;
   }
   TRACE("phLibNfc_Mgt_ConfigureDiscovery callback returned 0x%08x ", nfc_jni_cb_status);

} 

static void nfc_jni_stop_discovery_locked(struct nfc_jni_native_data *nat)
{
   phLibNfc_sADD_Cfg_t discovery_cfg;
   NFCSTATUS ret;

   discovery_cfg.PollDevInfo.PollEnabled = 0;
   discovery_cfg.Duration = 0xffffffff;
   /*discovery_cfg.NfcIP_Mode = phNfc_eInvalidP2PMode;*/
   discovery_cfg.NfcIP_Mode = phNfc_eDefaultP2PMode;
   discovery_cfg.NfcIP_Tgt_Disable = TRUE;
 
   /* Start Polling loop */
   TRACE("******  Stop NFC Discovery ******");
   REENTRANCE_LOCK();
   ret = phLibNfc_Mgt_ConfigureDiscovery(NFC_DISCOVERY_CONFIG,discovery_cfg, nfc_jni_discover_callback, (void *)nat);
   REENTRANCE_UNLOCK();
   TRACE("phLibNfc_Mgt_ConfigureDiscovery(%s-%s-%s-%s-%s-%s, %s-%x-%x) returned 0x%08x\n",
      discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443A==TRUE?"3A":"",
      discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443B==TRUE?"3B":"",
      discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica212==TRUE?"F2":"",
      discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica424==TRUE?"F4":"",
      discovery_cfg.PollDevInfo.PollCfgInfo.EnableNfcActive==TRUE?"NFC":"",
      discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso15693==TRUE?"RFID":"",
      discovery_cfg.PollDevInfo.PollCfgInfo.DisableCardEmulation==FALSE?"CE":"",
      discovery_cfg.NfcIP_Mode, discovery_cfg.Duration, ret);

   if(ret != NFCSTATUS_PENDING)
   {
        emergency_recovery(nat);
   }

   /* Wait for callback response */
   sem_wait(nfc_jni_manager_sem);
   if(nfc_jni_cb_status != NFCSTATUS_SUCCESS)
   {
      LOGD("phLibNfc_Mgt_ConfigureDiscovery callback returned 0x%08x ", nfc_jni_cb_status);
      return;
   }
   TRACE("phLibNfc_Mgt_ConfigureDiscovery callback returned 0x%08x ", nfc_jni_cb_status);

} 


static void nfc_jni_disconnect_callback(void *pContext,
   phLibNfc_Handle hRemoteDev, NFCSTATUS status)
{
   LOG_CALLBACK("nfc_jni_disconnect_callback", status);

   nfc_jni_cb_status = status;

   sem_post(nfc_jni_manager_sem);
}

static void com_android_nfc_NfcManager_doDisconnectTag()
{
   NFCSTATUS status;
   jboolean result = JNI_FALSE;

   /* Disconnect */
   TRACE("Disconnecting from tag (%x)", storedHandle);

    TRACE("phLibNfc_RemoteDev_Disconnect(%x)", storedHandle);
    REENTRANCE_LOCK();
    status = phLibNfc_RemoteDev_Disconnect(storedHandle, NFC_DISCOVERY_CONTINUE,
                                           nfc_jni_disconnect_callback, &storedHandle);
    REENTRANCE_UNLOCK();
    if(status != NFCSTATUS_PENDING)
    {
        LOGE("phLibNfc_RemoteDev_Disconnect(%x) returned 0x%04x[%s]", storedHandle, status, nfc_jni_get_status_name(status));
        /* Reset stored handle */
        storedHandle = 0;
        return;
    }
    TRACE("phLibNfc_RemoteDev_Disconnect(%x) returned 0x%04x[%s]", storedHandle, status, nfc_jni_get_status_name(status));

    /* Wait for callback response */
    sem_wait(nfc_jni_manager_sem);

    /* Disconnect Status */
    if(nfc_jni_cb_status != NFCSTATUS_SUCCESS)
    {
        TRACE("phLibNfc_RemoteDev_Disconnect(%x) returned 0x%04x[%s]", storedHandle, nfc_jni_cb_status, nfc_jni_get_status_name(nfc_jni_cb_status));
        /* Reset stored handle */
        storedHandle = 0;
        return;
    }
    TRACE("phLibNfc_RemoteDev_Disconnect(%x) returned 0x%04x[%s]", storedHandle, nfc_jni_cb_status, nfc_jni_get_status_name(nfc_jni_cb_status));

    /* Reset stored handle */
    storedHandle = 0;
}


static void com_android_nfc_NfcManager_disableDiscovery(JNIEnv *e, jobject o)
{
    struct nfc_jni_native_data *nat;

    CONCURRENCY_LOCK();

    /* Retrieve native structure address */
    nat = nfc_jni_get_nat(e, o);
   
    nfc_jni_stop_discovery_locked(nat);

    if(storedHandle != 0)
    {
        TRACE("Disconnect connected TAG");
        com_android_nfc_NfcManager_doDisconnectTag();
    }

    CONCURRENCY_UNLOCK();
}
    
static void com_android_nfc_NfcManager_enableDiscovery(
   JNIEnv *e, jobject o, jint mode)
{
   NFCSTATUS ret;
   struct nfc_jni_native_data *nat;

   CONCURRENCY_LOCK();
   
   /* Retrieve native structure address */
   nat = nfc_jni_get_nat(e, o); 
   
   if(mode == DISCOVERY_MODE_TAG_READER)
   {
      nfc_jni_start_discovery_locked(nat);
   }
   else if(DISCOVERY_MODE_CARD_EMULATION)
   {
      nfc_jni_start_card_emu_discovery_locked(nat);
   }

   CONCURRENCY_UNLOCK();
}


static jboolean com_android_nfc_NfcManager_init_native_struc(JNIEnv *e, jobject o)
{
   NFCSTATUS status;
   struct nfc_jni_native_data *nat = NULL;
   jclass cls;
   jobject obj;
   jfieldID f;

   TRACE("******  Init Native Structure ******"); 

   /* Initialize native structure */
   nat = (nfc_jni_native_data*)malloc(sizeof(struct nfc_jni_native_data));
   if(nat == NULL)
   {
      LOGD("malloc of nfc_jni_native_data failed");
      return FALSE;   
   }
   memset(nat, 0, sizeof(*nat));
   e->GetJavaVM(&(nat->vm));
   nat->env_version = e->GetVersion();
   nat->manager = e->NewGlobalRef(o);
      
   cls = e->GetObjectClass(o);
   f = e->GetFieldID(cls, "mNative", "I");
   e->SetIntField(o, f, (jint)nat);
                 
   /* Initialize native cached references */
   cached_NfcManager_notifyNdefMessageListeners = e->GetMethodID(cls,
      "notifyNdefMessageListeners","(Lcom/android/nfc/NativeNfcTag;)V");

   cached_NfcManager_notifyTransactionListeners = e->GetMethodID(cls,
      "notifyTransactionListeners", "([B)V");
         
   cached_NfcManager_notifyLlcpLinkActivation = e->GetMethodID(cls,
      "notifyLlcpLinkActivation","(Lcom/android/nfc/NativeP2pDevice;)V");
         
   cached_NfcManager_notifyLlcpLinkDeactivated = e->GetMethodID(cls,
      "notifyLlcpLinkDeactivated","(Lcom/android/nfc/NativeP2pDevice;)V"); 
      
   cached_NfcManager_notifyTargetDeselected = e->GetMethodID(cls,
      "notifyTargetDeselected","()V"); 
      
      
   if(nfc_jni_cache_object(e,"com/android/nfc/NativeNfcTag",&(nat->cached_NfcTag)) == -1)
   {
      LOGD("Native Structure initialization failed");
      return FALSE;
   }
         
   if(nfc_jni_cache_object(e,"com/android/nfc/NativeP2pDevice",&(nat->cached_P2pDevice)) == -1)
   {
      LOGD("Native Structure initialization failed");
      return FALSE;   
   }

   TRACE("****** Init Native Structure OK ******"); 
   return TRUE;
}
 
/* Init/Deinit method */
static jboolean com_android_nfc_NfcManager_initialize(JNIEnv *e, jobject o)
{
   struct nfc_jni_native_data *nat = NULL;
   int init_result = JNI_FALSE;
#ifdef TNFC_EMULATOR_ONLY
   char value[PROPERTY_VALUE_MAX];
#endif
   jboolean result;
   
   CONCURRENCY_LOCK();

#ifdef TNFC_EMULATOR_ONLY
   if (!property_get("ro.kernel.qemu", value, 0))
   {
      LOGE("NFC Initialization failed: not running in an emulator\n");
      goto clean_and_return;
   }
#endif

   /* Retrieve native structure address */
   nat = nfc_jni_get_nat(e, o); 
   exported_nat = nat;

   /* Perform the initialization */
   init_result = nfc_jni_initialize(nat);

clean_and_return:
   CONCURRENCY_UNLOCK();

   /* Convert the result and return */
   return (init_result==TRUE)?JNI_TRUE:JNI_FALSE;
}

static jboolean com_android_nfc_NfcManager_deinitialize(JNIEnv *e, jobject o)
{
   struct timespec ts;
   NFCSTATUS status;
   struct nfc_jni_native_data *nat;
   int bStackReset = FALSE;

   /* Retrieve native structure address */
   nat = nfc_jni_get_nat(e, o); 

   /* Clear previous configuration */
   memset(&nat->discovery_cfg, 0, sizeof(phLibNfc_sADD_Cfg_t));
   memset(&nat->registry_info, 0, sizeof(phLibNfc_Registry_Info_t));
   
   TRACE("phLibNfc_Mgt_DeInitialize()");
   REENTRANCE_LOCK();
   status = phLibNfc_Mgt_DeInitialize(gHWRef, nfc_jni_deinit_callback, (void *)nat);
   REENTRANCE_UNLOCK();
   if (status == NFCSTATUS_PENDING)
   {
      TRACE("phLibNfc_Mgt_DeInitialize() returned 0x%04x[%s]", status, nfc_jni_get_status_name(status));

      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += 5;

      /* Wait for callback response */
      if(sem_timedwait(nfc_jni_manager_sem, &ts) == -1)
      {
         LOGW("Operation timed out");
         bStackReset = TRUE;
      }
   }
   else
   {
      TRACE("phLibNfc_Mgt_DeInitialize() returned 0x%04x[%s]", status, nfc_jni_get_status_name(status));
      bStackReset = TRUE;
   }

   if(bStackReset == TRUE)
   {
      /* Complete deinit. failed, try hard restart of NFC */
      LOGW("Reseting stack...");
      emergency_recovery(nat);
   }

   /* Unconfigure driver */
   TRACE("phLibNfc_Mgt_UnConfigureDriver()");
   REENTRANCE_LOCK();
   status = phLibNfc_Mgt_UnConfigureDriver(gHWRef);
   REENTRANCE_UNLOCK();
   if(status != NFCSTATUS_SUCCESS)
   {
      LOGE("phLibNfc_Mgt_UnConfigureDriver() returned 0x%04x[%s]", status, nfc_jni_get_status_name(status));
   }
   else
   {
      LOGD("phLibNfc_Mgt_UnConfigureDriver() returned 0x%04x[%s]", status, nfc_jni_get_status_name(status));
   }

   TRACE("NFC Deinitialized");

   return TRUE;
}

/* Secure Element methods */
static jintArray com_android_nfc_NfcManager_doGetSecureElementList(JNIEnv *e, jobject o)
{
   NFCSTATUS ret;
   jintArray list= NULL;
   phLibNfc_SE_List_t se_list[PHLIBNFC_MAXNO_OF_SE];
   uint8_t i, se_count = PHLIBNFC_MAXNO_OF_SE;
    
   TRACE("******  Get Secure Element List ******");  
   
   TRACE("phLibNfc_SE_GetSecureElementList()");
   REENTRANCE_LOCK();
   ret = phLibNfc_SE_GetSecureElementList(se_list, &se_count);
   REENTRANCE_UNLOCK();
   if(ret != NFCSTATUS_SUCCESS)
   {
      LOGE("phLibNfc_SE_GetSecureElementList() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));
      return list;  
   }
   TRACE("phLibNfc_SE_GetSecureElementList() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));

   TRACE("Nb SE: %d", se_count);
   list =e->NewIntArray(se_count);
   for(i=0;i<se_count;i++)
   {
      if (se_list[i].eSE_Type == phLibNfc_SE_Type_SmartMX)
      {
        LOGD("phLibNfc_SE_GetSecureElementList(): SMX detected"); 
        LOGD("SE ID #%d: 0x%08x", i, se_list[i].hSecureElement);
      }
      else if(se_list[i].eSE_Type == phLibNfc_SE_Type_UICC)
      {
        LOGD("phLibNfc_SE_GetSecureElementList(): UICC detected");
        LOGD("SE ID #%d: 0x%08x", i, se_list[i].hSecureElement); 
      }
      
      e->SetIntArrayRegion(list, i, 1, (jint*)&se_list[i].hSecureElement);
   }

   e->DeleteLocalRef(list);
  
   return list;
}

static void com_android_nfc_NfcManager_doSelectSecureElement(JNIEnv *e, jobject o, jint seID)
{
   NFCSTATUS ret;
   struct nfc_jni_native_data *nat;

   CONCURRENCY_LOCK();
   
   /* Retrieve native structure address */
   nat = nfc_jni_get_nat(e, o);
   nat->seId = seID;

   TRACE("******  Select Secure Element ******"); 

   TRACE("phLibNfc_SE_SetMode(0x%08x, ...)", seID);
   /* Set SE mode - Virtual */
   REENTRANCE_LOCK();
   ret = phLibNfc_SE_SetMode(seID,phLibNfc_SE_ActModeVirtual, nfc_jni_se_set_mode_callback,(void *)nat);    
   REENTRANCE_UNLOCK();
   if(ret != NFCSTATUS_PENDING)
   {
      LOGD("phLibNfc_SE_SetMode() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));
      goto clean_and_return;
   }
   TRACE("phLibNfc_SE_SetMode() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));

   /* Wait for callback response */
   sem_wait(nfc_jni_manager_sem);

clean_and_return:
   CONCURRENCY_UNLOCK();
}

static void com_android_nfc_NfcManager_doDeselectSecureElement(JNIEnv *e, jobject o, jint seID)
{
   NFCSTATUS ret;
   struct nfc_jni_native_data *nat;

   CONCURRENCY_LOCK();
   
   /* Retrieve native structure address */
   nat = nfc_jni_get_nat(e, o);
   nat->seId = 0;

   TRACE("******  Deselect Secure Element ******"); 

   TRACE("phLibNfc_SE_SetMode(0x%02x, ...)", seID);
   /* Set SE mode - Off */
   REENTRANCE_LOCK();
   ret = phLibNfc_SE_SetMode(seID,phLibNfc_SE_ActModeOff, nfc_jni_se_set_mode_callback,(void *)nat);
   REENTRANCE_UNLOCK();
       
   TRACE("phLibNfc_SE_SetMode for SE 0x%02x returned 0x%02x",seID, ret);
   if(ret != NFCSTATUS_PENDING)
   {
      LOGE("phLibNfc_SE_SetMode() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));
      goto clean_and_return;
   }
   TRACE("phLibNfc_SE_SetMode() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));

   /* Wait for callback response */
   sem_wait(nfc_jni_manager_sem);

clean_and_return:
   CONCURRENCY_UNLOCK();
}


static void com_android_nfc_NfcManager_doCancel(JNIEnv *e, jobject o)
{
   struct nfc_jni_native_data *nat;

   /* Retrieve native structure address */
   nat = nfc_jni_get_nat(e, o);
  
   nat->status = NFCSTATUS_FAILED;
   sem_post(nfc_jni_open_sem);
}

/* Llcp methods */

static jboolean com_android_nfc_NfcManager_doCheckLlcp(JNIEnv *e, jobject o)
{
   NFCSTATUS ret;
   jboolean result = JNI_FALSE;
   struct nfc_jni_native_data *nat;
   
   CONCURRENCY_LOCK();

   /* Retrieve native structure address */
   nat = nfc_jni_get_nat(e, o);
   
   /* Check LLCP compliancy */
   TRACE("phLibNfc_Llcp_CheckLlcp(hLlcpHandle=0x%08x)", hLlcpHandle);
   REENTRANCE_LOCK();
   ret = phLibNfc_Llcp_CheckLlcp(hLlcpHandle,
                                 nfc_jni_checkLlcp_callback,
                                 nfc_jni_llcp_linkStatus_callback,
                                 (void*)nat);
   REENTRANCE_UNLOCK();
   /* In case of a NFCIP return NFCSTATUS_SUCCESS and in case of an another protocol
    * NFCSTATUS_PENDING. In this case NFCSTATUS_SUCCESS will also cause the callback. */
   if(ret != NFCSTATUS_PENDING && ret != NFCSTATUS_SUCCESS)
   {
      LOGE("phLibNfc_Llcp_CheckLlcp() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));
      goto clean_and_return;
   }
   TRACE("phLibNfc_Llcp_CheckLlcp() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));
                                    
   /* Wait for callback response */
   sem_wait(nfc_jni_llcp_sem);

   if(nfc_jni_cb_status == NFCSTATUS_SUCCESS)
   {
      result = JNI_TRUE;
   }

clean_and_return:
   CONCURRENCY_UNLOCK();
   return result;
}

static jboolean com_android_nfc_NfcManager_doActivateLlcp(JNIEnv *e, jobject o)
{
   NFCSTATUS ret;
   TRACE("phLibNfc_Llcp_Activate(hRemoteDevice=0x%08x)", hLlcpHandle);
   REENTRANCE_LOCK();
   ret = phLibNfc_Llcp_Activate(hLlcpHandle);
   REENTRANCE_UNLOCK();
   if(ret == NFCSTATUS_SUCCESS)
   {
      TRACE("phLibNfc_Llcp_Activate() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));
      return JNI_TRUE;
   }
   else
   {
      LOGE("phLibNfc_Llcp_Activate() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));
      return JNI_FALSE;   
   }
}



static jobject com_android_nfc_NfcManager_doCreateLlcpConnectionlessSocket(JNIEnv *e, jobject o, jint nSap)
{
   NFCSTATUS ret;
   jobject connectionlessSocket = NULL;
   phLibNfc_Handle hLlcpSocket;
   struct nfc_jni_native_data *nat;
   jclass clsNativeConnectionlessSocket;
   jfieldID f;
   
   /* Retrieve native structure address */
   nat = nfc_jni_get_nat(e, o); 
   
   /* Create socket */
   TRACE("phLibNfc_Llcp_Socket(hRemoteDevice=0x%08x, eType=phFriNfc_LlcpTransport_eConnectionLess, ...)", hLlcpHandle);
   REENTRANCE_LOCK();
   ret = phLibNfc_Llcp_Socket(hLlcpHandle,
                              phFriNfc_LlcpTransport_eConnectionLess,
                              NULL,
                              NULL,
                              &hLlcpSocket,
                              nfc_jni_llcp_transport_socket_err_callback,
                              (void*)nat);
   REENTRANCE_UNLOCK();
 
   if(ret != NFCSTATUS_SUCCESS)
   {
      lastErrorStatus = ret;
      LOGE("phLibNfc_Llcp_Socket() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));
      return NULL;
   }
   TRACE("phLibNfc_Llcp_Socket() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));
   
   
   /* Bind socket */
   TRACE("phLibNfc_Llcp_Bind(hSocket=0x%08x, nSap=0x%02x)", hLlcpSocket, nSap);
   REENTRANCE_LOCK();
   ret = phLibNfc_Llcp_Bind(hLlcpSocket,nSap);
   REENTRANCE_UNLOCK();
   if(ret != NFCSTATUS_SUCCESS)
   {
      lastErrorStatus = ret;
      LOGE("phLibNfc_Llcp_Bind() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));
      /* Close socket created */
      REENTRANCE_LOCK();
      ret = phLibNfc_Llcp_Close(hLlcpSocket); 
      REENTRANCE_UNLOCK();
      return NULL;
   }
   TRACE("phLibNfc_Llcp_Bind() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));
 
   
   /* Create new NativeLlcpConnectionlessSocket object */
   if(nfc_jni_cache_object(e,"com/android/nfc/NativeLlcpConnectionlessSocket",&(connectionlessSocket)) == -1)
   {
      return NULL;           
   } 
   
   /* Get NativeConnectionless class object */
   clsNativeConnectionlessSocket = e->GetObjectClass(connectionlessSocket);
   if(e->ExceptionCheck())
   {
      return NULL;  
   }
   
   /* Set socket handle */
   f = e->GetFieldID(clsNativeConnectionlessSocket, "mHandle", "I");
   e->SetIntField(connectionlessSocket, f,(jint)hLlcpSocket);
   TRACE("Connectionless socket Handle = %02x\n",hLlcpSocket);  
   
   /* Set the miu link of the connectionless socket */
   f = e->GetFieldID(clsNativeConnectionlessSocket, "mLinkMiu", "I");
   e->SetIntField(connectionlessSocket, f,(jint)PHFRINFC_LLCP_MIU_DEFAULT);
   TRACE("Connectionless socket Link MIU = %d\n",PHFRINFC_LLCP_MIU_DEFAULT);  
   
   /* Set socket SAP */
   f = e->GetFieldID(clsNativeConnectionlessSocket, "mSap", "I");
   e->SetIntField(connectionlessSocket, f,(jint)nSap);
   TRACE("Connectionless socket SAP = %d\n",nSap);  
   
   return connectionlessSocket;
}

static jobject com_android_nfc_NfcManager_doCreateLlcpServiceSocket(JNIEnv *e, jobject o, jint nSap, jstring sn, jint miu, jint rw, jint linearBufferLength)
{
   NFCSTATUS ret;
   phLibNfc_Handle hLlcpSocket;
   phLibNfc_Llcp_sSocketOptions_t sOptions;
   phNfc_sData_t sWorkingBuffer;
   phNfc_sData_t serviceName;
   struct nfc_jni_native_data *nat;
   jobject serviceSocket = NULL;
   jclass clsNativeLlcpServiceSocket;
   jfieldID f;  
  
   /* Retrieve native structure address */
   nat = nfc_jni_get_nat(e, o); 
   
   /* Set Connection Oriented socket options */
   sOptions.miu = miu;
   sOptions.rw  = rw;  
  
   /* Allocate Working buffer length */
   sWorkingBuffer.length = (miu*rw)+ miu + linearBufferLength;
   sWorkingBuffer.buffer = (uint8_t*)malloc(sWorkingBuffer.length);

   
   /* Create socket */
   TRACE("phLibNfc_Llcp_Socket(hRemoteDevice=0x%08x, eType=phFriNfc_LlcpTransport_eConnectionOriented, ...)", hLlcpHandle);
   REENTRANCE_LOCK();
   ret = phLibNfc_Llcp_Socket(hLlcpHandle,
                              phFriNfc_LlcpTransport_eConnectionOriented,
                              &sOptions,
                              &sWorkingBuffer,
                              &hLlcpSocket,
                              nfc_jni_llcp_transport_socket_err_callback,
                              (void*)nat);
   REENTRANCE_UNLOCK();
                                                     
   if(ret != NFCSTATUS_SUCCESS)
   {
      LOGE("phLibNfc_Llcp_Socket() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));
      lastErrorStatus = ret;
      return NULL;
   }
   TRACE("phLibNfc_Llcp_Socket() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));
   
   /* Bind socket */
   TRACE("phLibNfc_Llcp_Bind(hSocket=0x%08x, nSap=0x%02x)", hLlcpSocket, nSap);
   REENTRANCE_LOCK();
   ret = phLibNfc_Llcp_Bind(hLlcpSocket,nSap);
   REENTRANCE_UNLOCK();
   if(ret != NFCSTATUS_SUCCESS)
   {
      lastErrorStatus = ret;
      LOGE("phLibNfc_Llcp_Bind() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));
      /* Close socket created */
      ret = phLibNfc_Llcp_Close(hLlcpSocket); 
      return NULL;
   }
   TRACE("phLibNfc_Llcp_Bind() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));

   /* Service socket */
   if (sn == NULL) {
       serviceName.buffer = NULL;
       serviceName.length = 0;
   } else {
       serviceName.buffer = (uint8_t*)e->GetStringUTFChars(sn, NULL);
       serviceName.length = (uint32_t)e->GetStringUTFLength(sn);
   }

   TRACE("phLibNfc_Llcp_Listen(hSocket=0x%08x, ...)", hLlcpSocket);
   REENTRANCE_LOCK();
   ret = phLibNfc_Llcp_Listen( hLlcpSocket,
                               &serviceName,
                               nfc_jni_llcp_transport_listen_socket_callback,
                               (void*)nat);
   REENTRANCE_UNLOCK();
                               
   if(ret != NFCSTATUS_SUCCESS)
   {
      LOGE("phLibNfc_Llcp_Listen() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));
      lastErrorStatus = ret;
      /* Close created socket */
      REENTRANCE_LOCK();
      ret = phLibNfc_Llcp_Close(hLlcpSocket); 
      REENTRANCE_UNLOCK();
      return NULL;
   }                         
   TRACE("phLibNfc_Llcp_Listen() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));
   
   /* Create new NativeLlcpServiceSocket object */
   if(nfc_jni_cache_object(e,"com/android/nfc/NativeLlcpServiceSocket",&(serviceSocket)) == -1)
   {
      LOGE("Llcp Socket object creation error");
      return NULL;           
   } 
   
   /* Get NativeLlcpServiceSocket class object */
   clsNativeLlcpServiceSocket = e->GetObjectClass(serviceSocket);
   if(e->ExceptionCheck())
   {
      LOGE("Llcp Socket get object class error");
      return NULL;  
   } 
   
   /* Set socket handle */
   f = e->GetFieldID(clsNativeLlcpServiceSocket, "mHandle", "I");
   e->SetIntField(serviceSocket, f,(jint)hLlcpSocket);
   TRACE("Service socket Handle = %02x\n",hLlcpSocket);  
   
   /* Set socket linear buffer length */
   f = e->GetFieldID(clsNativeLlcpServiceSocket, "mLocalLinearBufferLength", "I");
   e->SetIntField(serviceSocket, f,(jint)linearBufferLength);
   TRACE("Service socket Linear buffer length = %02x\n",linearBufferLength);  
   
   /* Set socket MIU */
   f = e->GetFieldID(clsNativeLlcpServiceSocket, "mLocalMiu", "I");
   e->SetIntField(serviceSocket, f,(jint)miu);
   TRACE("Service socket MIU = %d\n",miu);  
   
   /* Set socket RW */
   f = e->GetFieldID(clsNativeLlcpServiceSocket, "mLocalRw", "I");
   e->SetIntField(serviceSocket, f,(jint)rw);
   TRACE("Service socket RW = %d\n",rw);   
  
   return serviceSocket;
}

static jobject com_android_nfc_NfcManager_doCreateLlcpSocket(JNIEnv *e, jobject o, jint nSap, jint miu, jint rw, jint linearBufferLength)
{
   jobject clientSocket = NULL;
   NFCSTATUS ret;
   phLibNfc_Handle hLlcpSocket;
   phLibNfc_Llcp_sSocketOptions_t sOptions;
   phNfc_sData_t sWorkingBuffer;
   struct nfc_jni_native_data *nat;
   jclass clsNativeLlcpSocket;
   jfieldID f;
   
   /* Retrieve native structure address */
   nat = nfc_jni_get_nat(e, o); 
   
   /* Set Connection Oriented socket options */
   sOptions.miu = miu;
   sOptions.rw  = rw;
   
   /* Allocate Working buffer length */
   sWorkingBuffer.length = (miu*rw)+ miu + linearBufferLength;
   sWorkingBuffer.buffer = (uint8_t*)malloc(sWorkingBuffer.length);

   /* Create socket */
   TRACE("phLibNfc_Llcp_Socket(hRemoteDevice=0x%08x, eType=phFriNfc_LlcpTransport_eConnectionOriented, ...)", hLlcpHandle);
   REENTRANCE_LOCK();
   ret = phLibNfc_Llcp_Socket(hLlcpHandle,
                              phFriNfc_LlcpTransport_eConnectionOriented,
                              &sOptions,
                              &sWorkingBuffer,
                              &hLlcpSocket,
                              nfc_jni_llcp_transport_socket_err_callback,
                              (void*)nat);
   REENTRANCE_UNLOCK();

   if(ret != NFCSTATUS_SUCCESS)
   {
      LOGE("phLibNfc_Llcp_Socket() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));
      lastErrorStatus = ret;
      return NULL;
   }
   TRACE("phLibNfc_Llcp_Socket() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));
   
   /* Create new NativeLlcpSocket object */
   if(nfc_jni_cache_object(e,"com/android/nfc/NativeLlcpSocket",&(clientSocket)) == -1)
   {
      LOGE("Llcp socket object creation error");  
      return NULL;           
   } 
   
   /* Get NativeConnectionless class object */
   clsNativeLlcpSocket = e->GetObjectClass(clientSocket);
   if(e->ExceptionCheck())
   {
      LOGE("Get class object error");    
      return NULL;  
   }
   
   /* Test if an SAP number is present */
   if(nSap != 0)
   {
      /* Bind socket */
      TRACE("phLibNfc_Llcp_Bind(hSocket=0x%08x, nSap=0x%02x)", hLlcpSocket, nSap);
      REENTRANCE_LOCK();
      ret = phLibNfc_Llcp_Bind(hLlcpSocket,nSap);
      REENTRANCE_UNLOCK();
      if(ret != NFCSTATUS_SUCCESS)
      {
         lastErrorStatus = ret;
         LOGE("phLibNfc_Llcp_Bind() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));
         /* Close socket created */
         REENTRANCE_LOCK();
         ret = phLibNfc_Llcp_Close(hLlcpSocket); 
         REENTRANCE_UNLOCK();
         return NULL;
      }
      TRACE("phLibNfc_Llcp_Bind() returned 0x%04x[%s]", ret, nfc_jni_get_status_name(ret));
      
      /* Set socket SAP */
      f = e->GetFieldID(clsNativeLlcpSocket, "mSap", "I");
      e->SetIntField(clientSocket, f,(jint)nSap);
      TRACE("socket SAP = %d\n",nSap);    
   }  
      
   /* Set socket handle */
   f = e->GetFieldID(clsNativeLlcpSocket, "mHandle", "I");
   e->SetIntField(clientSocket, f,(jint)hLlcpSocket);
   TRACE("socket Handle = %02x\n",hLlcpSocket);  
   
   /* Set socket MIU */
   f = e->GetFieldID(clsNativeLlcpSocket, "mLocalMiu", "I");
   e->SetIntField(clientSocket, f,(jint)miu);
   TRACE("socket MIU = %d\n",miu);  
   
   /* Set socket RW */
   f = e->GetFieldID(clsNativeLlcpSocket, "mLocalRw", "I");
   e->SetIntField(clientSocket, f,(jint)rw);
   TRACE("socket RW = %d\n",rw);   
   
  
   return clientSocket;
}

static jint com_android_nfc_NfcManager_doGetLastError(JNIEnv *e, jobject o)
{
   TRACE("Last Error Status = 0x%02x",lastErrorStatus);
   
   if(lastErrorStatus == NFCSTATUS_BUFFER_TOO_SMALL)
   {
      return ERROR_BUFFER_TOO_SMALL;
   }
   else if(lastErrorStatus == NFCSTATUS_INSUFFICIENT_RESOURCES)
   {
      return  ERROR_INSUFFICIENT_RESOURCES;
   }
   else
   {
      return lastErrorStatus;
   }
}

static void com_android_nfc_NfcManager_doSetProperties(JNIEnv *e, jobject o, jint param, jint value)
{  
   NFCSTATUS ret;
   struct nfc_jni_native_data *nat;

   /* Retrieve native structure address */
   nat = nfc_jni_get_nat(e, o);
   
   switch(param)
   {
   case PROPERTY_LLCP_LTO:
      {
         TRACE("> Set LLCP LTO to %d",value); 
         nat->lto = value;
      }break;
      
   case PROPERTY_LLCP_MIU:
      {
         TRACE("> Set LLCP MIU to %d",value);  
         nat->miu = value;
      }break;
      
   case PROPERTY_LLCP_WKS:
      {
         TRACE("> Set LLCP WKS to %d",value); 
         nat->wks = value;
      }break;
      
   case PROPERTY_LLCP_OPT:
      {
         TRACE("> Set LLCP OPT to %d",value); 
         nat->opt = value;    
      }break;
      
   case PROPERTY_NFC_DISCOVERY_A:
      {
         TRACE("> Set NFC DISCOVERY A to %d",value); 
         nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443A = value;  
      }break;
      
   case PROPERTY_NFC_DISCOVERY_B:
      {
         TRACE("> Set NFC DISCOVERY B to %d",value); 
         nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443B = value;    
      }break;
      
   case PROPERTY_NFC_DISCOVERY_F:
      {
         TRACE("> Set NFC DISCOVERY F to %d",value); 
         nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica212 = value;
         nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica424 = value;
      }break;
      
   case PROPERTY_NFC_DISCOVERY_15693:
      {
         TRACE("> Set NFC DISCOVERY 15693 to %d",value); 
         nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso15693 = value; 
      }break;
      
   case PROPERTY_NFC_DISCOVERY_NCFIP:
      {
         TRACE("> Set NFC DISCOVERY NFCIP to %d",value); 
         nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableNfcActive = value; 
      }break;
   default:
      {
         TRACE("> Unknown Property "); 
      }break;
   }
   

}
/*
 * JNI registration.
 */
static JNINativeMethod gMethods[] =
{
   {"initializeNativeStructure", "()Z",
      (void *)com_android_nfc_NfcManager_init_native_struc},
      
   {"initialize", "()Z",
      (void *)com_android_nfc_NfcManager_initialize},
 
   {"deinitialize", "()Z",
      (void *)com_android_nfc_NfcManager_deinitialize},
      
   {"enableDiscovery", "(I)V",
      (void *)com_android_nfc_NfcManager_enableDiscovery},

   {"doGetSecureElementList", "()[I",
      (void *)com_android_nfc_NfcManager_doGetSecureElementList},
      
   {"doSelectSecureElement", "(I)V",
      (void *)com_android_nfc_NfcManager_doSelectSecureElement},
      
   {"doDeselectSecureElement", "(I)V",
      (void *)com_android_nfc_NfcManager_doDeselectSecureElement},
      
   {"doCancel", "()V",
      (void *)com_android_nfc_NfcManager_doCancel},
      
   {"doCheckLlcp", "()Z",
      (void *)com_android_nfc_NfcManager_doCheckLlcp},
      
   {"doActivateLlcp", "()Z",
      (void *)com_android_nfc_NfcManager_doActivateLlcp},
            
   {"doCreateLlcpConnectionlessSocket", "(I)Lcom/android/nfc/NativeLlcpConnectionlessSocket;",
      (void *)com_android_nfc_NfcManager_doCreateLlcpConnectionlessSocket},
        
   {"doCreateLlcpServiceSocket", "(ILjava/lang/String;III)Lcom/android/nfc/NativeLlcpServiceSocket;",
      (void *)com_android_nfc_NfcManager_doCreateLlcpServiceSocket},
      
   {"doCreateLlcpSocket", "(IIII)Lcom/android/nfc/NativeLlcpSocket;",
      (void *)com_android_nfc_NfcManager_doCreateLlcpSocket},
      
   {"doGetLastError", "()I",
      (void *)com_android_nfc_NfcManager_doGetLastError},
      
   {"doSetProperties", "(II)V",
      (void *)com_android_nfc_NfcManager_doSetProperties},

   {"disableDiscovery", "()V",
      (void *)com_android_nfc_NfcManager_disableDiscovery},

};   
  
      
int register_com_android_nfc_NativeNfcManager(JNIEnv *e)
{
    nfc_jni_native_monitor_t *nfc_jni_native_monitor;

    nfc_jni_manager_sem = (sem_t *)malloc(sizeof(sem_t));
    nfc_jni_llcp_sem = (sem_t *)malloc(sizeof(sem_t));
    nfc_jni_open_sem = (sem_t *)malloc(sizeof(sem_t));
    nfc_jni_init_sem = (sem_t *)malloc(sizeof(sem_t));
    nfc_jni_ioctl_sem = (sem_t *)malloc(sizeof(sem_t));
    nfc_jni_llcp_listen_sem = (sem_t *)malloc(sizeof(sem_t));

   nfc_jni_native_monitor = nfc_jni_init_monitor();
   if(nfc_jni_native_monitor == NULL)
   {
      LOGE("NFC Manager cannot recover native monitor %x\n", errno);
      return -1;
   }

   if(sem_init(nfc_jni_manager_sem, 0, 0) == -1)
   {
      LOGE("NFC Manager Semaphore creation %x\n", errno);
      return -1;
   }

   if(sem_init(nfc_jni_llcp_sem, 0, 0) == -1)
   {
      LOGE("NFC Manager Semaphore creation %x\n", errno);
      return -1;
   }

   if(sem_init(nfc_jni_open_sem, 0, 0) == -1)
   {
      LOGE("NFC Open Semaphore creation %x\n", errno);
      return -1;
   }

   if(sem_init(nfc_jni_init_sem, 0, 0) == -1)
   {
      LOGE("NFC Init Semaphore creation %x\n", errno);
      return -1;
   }

   if(sem_init(nfc_jni_llcp_listen_sem, 0, 0) == -1)
   {
      LOGE("NFC Listen Semaphore creation %x\n", errno);
      return -1;
   }

   if(sem_init(nfc_jni_ioctl_sem, 0, 0) == -1)
   {
      LOGE("NFC IOCTL Semaphore creation %x\n", errno);
      return -1;
   }

   return jniRegisterNativeMethods(e,
      "com/android/nfc/NativeNfcManager",
      gMethods, NELEM(gMethods));
}

} /* namespace android */
