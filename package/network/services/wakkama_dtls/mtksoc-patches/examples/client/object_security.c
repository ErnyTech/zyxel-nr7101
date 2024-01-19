/*******************************************************************************
 *
 * Copyright (c) 2013, 2014 Intel Corporation and others.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v2.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    http://www.eclipse.org/legal/epl-v20.html
 * The Eclipse Distribution License is available at
 *    http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    David Navarro, Intel Corporation - initial API and implementation
 *    Bosch Software Innovations GmbH - Please refer to git log
 *    Pascal Rieux - Please refer to git log
 *    Ville Skyttä - Please refer to git log
 *    Scott Bertin, AMETEK, Inc. - Please refer to git log
 *
 *******************************************************************************/

/*
 *  Resources:
 *
 *          Name            | ID | Operations | Instances | Mandatory |  Type   |  Range  | Units |
 *  Server URI              |  0 |            |  Single   |    Yes    | String  |         |       |
 *  Bootstrap Server        |  1 |            |  Single   |    Yes    | Boolean |         |       |
 *  Security Mode           |  2 |            |  Single   |    Yes    | Integer |   0-3   |       |
 *  Public Key or ID        |  3 |            |  Single   |    Yes    | Opaque  |         |       |
 *  Server Public Key or ID |  4 |            |  Single   |    Yes    | Opaque  |         |       |
 *  Secret Key              |  5 |            |  Single   |    Yes    | Opaque  |         |       |
 *  SMS Security Mode       |  6 |            |  Single   |    No     | Integer |  0-255  |       |
 *  SMS Binding Key Param.  |  7 |            |  Single   |    No     | Opaque  |   6 B   |       |
 *  SMS Binding Secret Keys |  8 |            |  Single   |    No     | Opaque  | 32-48 B |       |
 *  Server SMS Number       |  9 |            |  Single   |    No     | String  |         |       |
 *  Short Server ID         | 10 |            |  Single   |    No     | Integer | 1-65535 |       |
 *  Client Hold Off Time    | 11 |            |  Single   |    No     | Integer |         |   s   |
 *  BS Account Timeout      | 12 |            |  Single   |    No     | Integer |         |   s   |
 *
 */

/*
 * Here we implement a very basic LWM2M Security Object which only knows NoSec security mode.
 */

#include "liblwm2m.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef WITH_TINYDTLS
#include "dtlsconnection.h"
#else
#include "connection.h"
#endif

#include "zcfg_debug.h"
#include "zcfg_common.h"
#include "zcfg_rdm_oid.h"
#include "zcfg_rdm_obj.h"
#include "zcfg_fe_rdm_access.h"
#include "zcfg_fe_rdm_struct.h"
#include "zcfg_eid.h"
#include "zcfg_net.h"


#include "zcmd_schema.h"
#include "zcfg_msg.h"
#include "zcmd_tool.h"
#include "zlog_api.h"
#include "json-c/json_object.h"

#define WAKKAMA_MAPPING_TABLE "/etc/zcfg_wakkama_mapping_table"

typedef struct _security_instance_
{
    struct _security_instance_ * next;        // matches lwm2m_list_t::next
    uint16_t                     instanceId;  // matches lwm2m_list_t::id
    char *                       uri;
    bool                         isBootstrap;
    uint8_t                      securityMode;
    char *                       publicIdentity;
    uint16_t                     publicIdLen;
    char *                       serverPublicKey;
    uint16_t                     serverPublicKeyLen;
    char *                       secretKey;
    uint16_t                     secretKeyLen;
    uint8_t                      smsSecurityMode;
    char *                       smsParams; // SMS binding key parameters
    uint16_t                     smsParamsLen;
    char *                       smsSecret; // SMS binding secret key
    uint16_t                     smsSecretLen;
    uint16_t                     shortID;
    uint32_t                     clientHoldOffTime;
    uint32_t                     bootstrapServerAccountTimeout;
} security_instance_t;

static uint8_t prv_get_value(lwm2m_data_t * dataP,
                             security_instance_t * targetP)
{
    switch (dataP->id)
    {
    case LWM2M_SECURITY_URI_ID:
        lwm2m_data_encode_string(targetP->uri, dataP);
        return COAP_205_CONTENT;

    case LWM2M_SECURITY_BOOTSTRAP_ID:
        lwm2m_data_encode_bool(targetP->isBootstrap, dataP);
        return COAP_205_CONTENT;

    case LWM2M_SECURITY_SECURITY_ID:
        lwm2m_data_encode_int(targetP->securityMode, dataP);
        return COAP_205_CONTENT;

    case LWM2M_SECURITY_PUBLIC_KEY_ID:
        lwm2m_data_encode_opaque((uint8_t*)targetP->publicIdentity, targetP->publicIdLen, dataP);
        return COAP_205_CONTENT;

    case LWM2M_SECURITY_SERVER_PUBLIC_KEY_ID:
        lwm2m_data_encode_opaque((uint8_t*)targetP->serverPublicKey, targetP->serverPublicKeyLen, dataP);
        return COAP_205_CONTENT;

    case LWM2M_SECURITY_SECRET_KEY_ID:
        lwm2m_data_encode_opaque((uint8_t*)targetP->secretKey, targetP->secretKeyLen, dataP);
        return COAP_205_CONTENT;

    case LWM2M_SECURITY_SMS_SECURITY_ID:
        lwm2m_data_encode_int(targetP->smsSecurityMode, dataP);
        return COAP_205_CONTENT;

    case LWM2M_SECURITY_SMS_KEY_PARAM_ID:
        lwm2m_data_encode_opaque((uint8_t*)targetP->smsParams, targetP->smsParamsLen, dataP);
        return COAP_205_CONTENT;

    case LWM2M_SECURITY_SMS_SECRET_KEY_ID:
        lwm2m_data_encode_opaque((uint8_t*)targetP->smsSecret, targetP->smsSecretLen, dataP);
        return COAP_205_CONTENT;

    case LWM2M_SECURITY_SMS_SERVER_NUMBER_ID:
        lwm2m_data_encode_int(0, dataP);
        return COAP_205_CONTENT;

    case LWM2M_SECURITY_SHORT_SERVER_ID:
        lwm2m_data_encode_int(targetP->shortID, dataP);
        return COAP_205_CONTENT;

    case LWM2M_SECURITY_HOLD_OFF_ID:
        lwm2m_data_encode_int(targetP->clientHoldOffTime, dataP);
        return COAP_205_CONTENT;

    case LWM2M_SECURITY_BOOTSTRAP_TIMEOUT_ID:
        lwm2m_data_encode_int(targetP->bootstrapServerAccountTimeout, dataP);
        return COAP_205_CONTENT;

    default:
        return COAP_404_NOT_FOUND;
    }
}

static uint8_t prv_security_read(lwm2m_context_t *contextP,
                                 uint16_t instanceId,
                                 int * numDataP,
                                 lwm2m_data_t ** dataArrayP,
                                 lwm2m_object_t * objectP)
{
    security_instance_t * targetP;
    uint8_t result;
    int i;

    int ret = ZCFG_SUCCESS;
    char omaID[16] = {0};
    char desValue[512] = {0};

    /* unused parameter */
    (void)contextP;

    targetP = (security_instance_t *)lwm2m_list_find(objectP->instanceList, instanceId);
    if (NULL == targetP) return COAP_404_NOT_FOUND;
	printf("%s() Jessie targetP!=NULL\n", __FUNCTION__);    // is the server asking for the full instance ?
    if (*numDataP == 0)
    {
        uint16_t resList[] = {LWM2M_SECURITY_URI_ID,
                              LWM2M_SECURITY_BOOTSTRAP_ID,
                              LWM2M_SECURITY_SECURITY_ID,
                              LWM2M_SECURITY_PUBLIC_KEY_ID,
                              LWM2M_SECURITY_SERVER_PUBLIC_KEY_ID,
                              LWM2M_SECURITY_SECRET_KEY_ID,
                              LWM2M_SECURITY_SMS_SECURITY_ID,
                              LWM2M_SECURITY_SMS_KEY_PARAM_ID,
                              LWM2M_SECURITY_SMS_SECRET_KEY_ID,
                              LWM2M_SECURITY_SMS_SERVER_NUMBER_ID,
                              LWM2M_SECURITY_SHORT_SERVER_ID,
                              LWM2M_SECURITY_HOLD_OFF_ID,
                              LWM2M_SECURITY_BOOTSTRAP_TIMEOUT_ID};
        int nbRes = sizeof(resList)/sizeof(uint16_t);

        *dataArrayP = lwm2m_data_new(nbRes);
        if (*dataArrayP == NULL) return COAP_500_INTERNAL_SERVER_ERROR;
        *numDataP = nbRes;
        for (i = 0 ; i < nbRes ; i++)
        {
            (*dataArrayP)[i].id = resList[i];
        }
    }

    i = 0;
    do
    {
#if 0 // original wakkama code
        if ((*dataArrayP)[i].type == LWM2M_TYPE_MULTIPLE_RESOURCE)
        {
            result = COAP_404_NOT_FOUND;
        }
        else
        {
            result = prv_get_value((*dataArrayP) + i, targetP);
        }
#else
        snprintf(omaID, 16, "%d_%d", objectP->objID, (*dataArrayP)[i].id);
				printf("*************[%s:%d]omaID = %s\n", __func__, __LINE__, omaID);
        switch((*dataArrayP)[i].id) {
            // String
            case LWM2M_SECURITY_URI_ID:
			{
                if ((*dataArrayP)[i].type == LWM2M_TYPE_MULTIPLE_RESOURCE) return COAP_404_NOT_FOUND;
				if((ret = prv_get_zcfg_value(omaID, instanceId, -1, desValue)) != ZCFG_SUCCESS)
				{
					printf("[%s]prv_get_zcfg_value fail, ret = %d!!!\n", __FUNCTION__, ret);
				}
				if(strlen(targetP->uri) > 0){
					lwm2m_data_encode_string(targetP->uri, *dataArrayP + i);
				}
				else{
					lwm2m_data_encode_string(desValue, *dataArrayP + i);
				}
				result = COAP_205_CONTENT;
				break;
            }

            //Opaque
            case LWM2M_SECURITY_PUBLIC_KEY_ID:
            case LWM2M_SECURITY_SERVER_PUBLIC_KEY_ID:
            case LWM2M_SECURITY_SECRET_KEY_ID:
            {
                if ((*dataArrayP)[i].type == LWM2M_TYPE_MULTIPLE_RESOURCE) return COAP_404_NOT_FOUND;
				if((ret = prv_get_zcfg_value(omaID, instanceId, -1, desValue)) != ZCFG_SUCCESS)
				{
					printf("[%s]prv_get_zcfg_value fail, ret = %d!!!\n", __FUNCTION__, ret);
				}
				//lwm2m_data_encode_string(desValue, *dataArrayP + i);
				lwm2m_data_encode_opaque((uint8_t*)desValue, strlen(desValue), *dataArrayP + i);//Jessie opaque
				result = COAP_205_CONTENT;
				break;
            }

            // Integer
            case LWM2M_SECURITY_SECURITY_ID:
            case LWM2M_SECURITY_SHORT_SERVER_ID:
            case LWM2M_SECURITY_HOLD_OFF_ID:
            case LWM2M_SECURITY_BOOTSTRAP_TIMEOUT_ID:
            {
                if ((*dataArrayP)[i].type == LWM2M_TYPE_MULTIPLE_RESOURCE) return COAP_404_NOT_FOUND;

                if((ret = prv_get_zcfg_value(omaID, instanceId, -1, desValue)) != ZCFG_SUCCESS)
                {
                    printf("[%s]prv_get_zcfg_value fail, ret = %d!!!\n", __FUNCTION__, ret);
                }

                lwm2m_data_encode_int(atoi(desValue), *dataArrayP + i);

                result = COAP_205_CONTENT;
                break;
            }

            // Boolean
            case LWM2M_SECURITY_BOOTSTRAP_ID:
            {
                if ((*dataArrayP)[i].type == LWM2M_TYPE_MULTIPLE_RESOURCE) return COAP_404_NOT_FOUND;

                if((ret = prv_get_zcfg_value(omaID, instanceId, -1, desValue)) != ZCFG_SUCCESS)
                {
                    printf("[%s]prv_get_zcfg_value fail, ret = %d!!!\n", __FUNCTION__, ret);
                }

                lwm2m_data_encode_bool((atoi(desValue)) ? true : false, *dataArrayP + i);

                result = COAP_205_CONTENT;
                break;
            }

            // case LWM2M_SECURITY_SMS_SECURITY_ID:
            // case LWM2M_SECURITY_SMS_KEY_PARAM_ID:
            // case LWM2M_SECURITY_SMS_SECRET_KEY_ID:
            // case LWM2M_SECURITY_SMS_SERVER_NUMBER_ID:

            default:
                result = COAP_404_NOT_FOUND;
        }
#endif

        i++;
    } while (i < *numDataP && result == COAP_205_CONTENT);

    return result;
}

#ifdef LWM2M_BOOTSTRAP

static uint8_t prv_security_write(lwm2m_context_t *contextP,
                                  uint16_t instanceId,
                                  int numData,
                                  lwm2m_data_t * dataArray,
                                  lwm2m_object_t * objectP,
                                  lwm2m_write_type_t writeType)
{
    security_instance_t * targetP;
    int i;
    uint8_t result = COAP_204_CHANGED;

    char param[512] = {0};
    char omaID[16] = {0};
    char valstr[32] = {0};
    int64_t iValue;
    uint64_t uValue;
    bool bValue = false;
    int jj,kk;

    /* unused parameter */
    (void)contextP;

    /* All write types are ignored. They don't apply during bootstrap. */
    (void)writeType;

    targetP = (security_instance_t *)lwm2m_list_find(objectP->instanceList, instanceId);
    if (NULL == targetP)
    {
        return COAP_404_NOT_FOUND;
    }

    i = 0;
    do {
#if 1 // original wakkama code
        /* No multiple instance resources */
        if (dataArray[i].type == LWM2M_TYPE_MULTIPLE_RESOURCE)
        {
            result = COAP_404_NOT_FOUND;
            continue;
        }
		
        switch (dataArray[i].id)
        {
        case LWM2M_SECURITY_URI_ID:
            if (targetP->uri != NULL) lwm2m_free(targetP->uri);
            targetP->uri = (char *)lwm2m_malloc(dataArray[i].value.asBuffer.length + 1);

            if (targetP->uri != NULL)
            {
                memset(targetP->uri, 0, dataArray[i].value.asBuffer.length + 1);
                strncpy(targetP->uri, (char*)dataArray[i].value.asBuffer.buffer, dataArray[i].value.asBuffer.length);
                result = COAP_204_CHANGED;
            }
            else
            {
                result = COAP_500_INTERNAL_SERVER_ERROR;
            }
            break;

        case LWM2M_SECURITY_BOOTSTRAP_ID:
            if (1 == lwm2m_data_decode_bool(dataArray + i, &(targetP->isBootstrap)))
            {
                result = COAP_204_CHANGED;
            }
            else
            {
                result = COAP_400_BAD_REQUEST;
            }
            break;

        case LWM2M_SECURITY_SECURITY_ID:
        {
            int64_t value;

            if (1 == lwm2m_data_decode_int(dataArray + i, &value))
            {
                if (value >= 0 && value <= 3)
                {
                    targetP->securityMode = value;
                    result = COAP_204_CHANGED;
                }
                else
                {
                    result = COAP_406_NOT_ACCEPTABLE;
                }
            }
            else
            {
                result = COAP_400_BAD_REQUEST;
            }
        }
        break;
        case LWM2M_SECURITY_PUBLIC_KEY_ID:
            if (targetP->publicIdentity != NULL) lwm2m_free(targetP->publicIdentity);
            targetP->publicIdentity = (char *)lwm2m_malloc(dataArray[i].value.asBuffer.length +1);

            if (targetP->publicIdentity != NULL)
            {
                memset(targetP->publicIdentity, 0, dataArray[i].value.asBuffer.length + 1);
                memcpy(targetP->publicIdentity, (char*)dataArray[i].value.asBuffer.buffer, dataArray[i].value.asBuffer.length);
                targetP->publicIdLen = dataArray[i].value.asBuffer.length;
                result = COAP_204_CHANGED;
            }
            else
            {
                result = COAP_500_INTERNAL_SERVER_ERROR;
            }
            break;

        case LWM2M_SECURITY_SERVER_PUBLIC_KEY_ID:
            if (targetP->serverPublicKey != NULL) lwm2m_free(targetP->serverPublicKey);
            targetP->serverPublicKey = (char *)lwm2m_malloc(dataArray[i].value.asBuffer.length +1);

            if (targetP->serverPublicKey != NULL)
            {
                memset(targetP->serverPublicKey, 0, dataArray[i].value.asBuffer.length + 1);
                memcpy(targetP->serverPublicKey, (char*)dataArray[i].value.asBuffer.buffer, dataArray[i].value.asBuffer.length);
                targetP->serverPublicKeyLen = dataArray[i].value.asBuffer.length;
                result = COAP_204_CHANGED;
            }
            else
            {
                result = COAP_500_INTERNAL_SERVER_ERROR;
            }
            break;

        case LWM2M_SECURITY_SECRET_KEY_ID:
			printf("%s() \n", __FUNCTION__);
            for(jj=0;jj<dataArray[i].value.asBuffer.length;jj++){
                //zcfgLogPrefix(ZCFG_LOG_INFO, ZCFG_LOGPRE_LWM2M, "write secret key %x.\n", dataArray[i].value.asBuffer.buffer[jj]);
				printf(" %x. ", dataArray[i].value.asBuffer.buffer[jj]);
            }
			printf("\n\n");

            if (targetP->secretKey != NULL) lwm2m_free(targetP->secretKey);
            targetP->secretKey = (char *)lwm2m_malloc(dataArray[i].value.asBuffer.length +1);

            if (targetP->secretKey != NULL)
            {
                memset(targetP->secretKey, 0, dataArray[i].value.asBuffer.length + 1);
                memcpy(targetP->secretKey, (char*)dataArray[i].value.asBuffer.buffer, dataArray[i].value.asBuffer.length);
                targetP->secretKeyLen = dataArray[i].value.asBuffer.length;
                result = COAP_204_CHANGED;
            }
            else
            {
                result = COAP_500_INTERNAL_SERVER_ERROR;
            }
            break;

        case LWM2M_SECURITY_SMS_SECURITY_ID:
            // Let just ignore this
            result = COAP_204_CHANGED;
            break;

        case LWM2M_SECURITY_SMS_KEY_PARAM_ID:
            // Let just ignore this
            result = COAP_204_CHANGED;
            break;

        case LWM2M_SECURITY_SMS_SECRET_KEY_ID:
            // Let just ignore this
            result = COAP_204_CHANGED;
            break;

        case LWM2M_SECURITY_SMS_SERVER_NUMBER_ID:
            // Let just ignore this
            result = COAP_204_CHANGED;
            break;

        case LWM2M_SECURITY_SHORT_SERVER_ID:
        {
            int64_t value;

            if (1 == lwm2m_data_decode_int(dataArray + i, &value))
            {
                if (value >= 0 && value <= 0xFFFF)
                {
                    targetP->shortID = value;
                    result = COAP_204_CHANGED;
                }
                else
                {
                    result = COAP_406_NOT_ACCEPTABLE;
                }
            }
            else
            {
                result = COAP_400_BAD_REQUEST;
            }
        }
        break;

        case LWM2M_SECURITY_HOLD_OFF_ID:
        {
            int64_t value;

            if (1 == lwm2m_data_decode_int(dataArray + i, &value))
            {
                if (value >= 0 && value <= UINT32_MAX)
                {
                    targetP->clientHoldOffTime = value;
                    result = COAP_204_CHANGED;
                }
                else
                {
                    result = COAP_406_NOT_ACCEPTABLE;
                }
            }
            else
            {
                result = COAP_400_BAD_REQUEST;
            }
            break;
        }

        case LWM2M_SECURITY_BOOTSTRAP_TIMEOUT_ID:
        {
            int64_t value;

            if (1 == lwm2m_data_decode_int(dataArray + i, &value))
            {
                if (value >= 0 && value <= UINT32_MAX)
                {
                    targetP->bootstrapServerAccountTimeout = value;
                    result = COAP_204_CHANGED;
                }
                else
                {
                    result = COAP_406_NOT_ACCEPTABLE;
                }
            }
            else
            {
                result = COAP_400_BAD_REQUEST;
            }
            break;
        }

        default:
            return COAP_400_BAD_REQUEST;
        }
//#else
        memset(param, 0, sizeof(param));
        snprintf(omaID, 16, "%d_%d", objectP->objID, (dataArray)[i].id);
        switch (dataArray[i].id)
        {
            // String
            case LWM2M_SECURITY_URI_ID:
            {
                strncpy(param, dataArray[i].value.asBuffer.buffer, dataArray[i].value.asBuffer.length);


                if(prv_set_zcfg_value(omaID, instanceId, -1, dataArray[i].type, param) == ZCFG_SUCCESS)
                {
                    result = COAP_204_CHANGED;
                }
                else
                {
                    result = COAP_400_BAD_REQUEST;
                }

                break;
            }

            //Opaque
            case LWM2M_SECURITY_PUBLIC_KEY_ID:
            case LWM2M_SECURITY_SERVER_PUBLIC_KEY_ID:
            case LWM2M_SECURITY_SECRET_KEY_ID:
            {
#if 0

				for (int j = 0, kk = 0; j < dataArray[i].value.asBuffer.length; j++, kk=kk+2) {
					snprintf(param+kk, sizeof(param), "%x", (dataArray[i].value.asBuffer.buffer[j] >> 4) & 0x0000000F);
					snprintf(param+kk+1, sizeof(param), "%x", dataArray[i].value.asBuffer.buffer[j] & 0x0000000F);
					zcfgLogPrefix(ZCFG_LOG_INFO, ZCFG_LOGPRE_LWM2M, "Write PSK ID or Key");
					zcfgLogPrefix(ZCFG_LOG_INFO, ZCFG_LOGPRE_LWM2M, " %c", param[kk]);
					zcfgLogPrefix(ZCFG_LOG_INFO, ZCFG_LOGPRE_LWM2M, " %c", param[kk+1]);
					printf("%s() Write PSK ID or Key ", __FUNCTION__);
					printf("param+kk=%c\n", param[kk]);
					printf("param+kk+1=%c\n", param[kk+1]);
				}
				//for debugging 
				printf("%s:%d param %s(No Skip conversion).\n", __func__, __LINE__, param);
#else
				//No need to convert it to ascii string from hex
                strncpy(param, dataArray[i].value.asBuffer.buffer, dataArray[i].value.asBuffer.length);
				//for debugging 
				//printf("[Marcus debug] In write %s:%d param %s(Skip conversion).\n", __func__, __LINE__, param);
#endif

				zcfgLogPrefix(ZCFG_LOG_INFO, ZCFG_LOGPRE_LWM2M, "Write PSK ID or Key %s to zcfg.\n", param);

				if(prv_set_zcfg_value(omaID, instanceId, -1, dataArray[i].type, param) == ZCFG_SUCCESS)
				{
					result = COAP_204_CHANGED;
				}
				else
				{
					result = COAP_400_BAD_REQUEST;
				}

                break;
            }
            // Interger
            case LWM2M_SECURITY_SECURITY_ID:
            case LWM2M_SECURITY_SHORT_SERVER_ID:
            case LWM2M_SECURITY_HOLD_OFF_ID:
            case LWM2M_SECURITY_BOOTSTRAP_TIMEOUT_ID:
            {
                lwm2m_data_decode_int(&dataArray[i], &iValue);

                sprintf(param, "%d", iValue);

                if(prv_set_zcfg_value(omaID, instanceId, -1, LWM2M_TYPE_INTEGER, param) == ZCFG_SUCCESS) // FIXME:dataArray[i].type  is LWM2M_TYPE_OPAQUE,but this case type should be LWM2M_TYPE_INTEGER, I dont know why
                {
                    result = COAP_204_CHANGED;
                }
                else
                {
                    result = COAP_400_BAD_REQUEST;
                }

                break;
            }

            // Boolean
            case LWM2M_SECURITY_BOOTSTRAP_ID:
            {
                lwm2m_data_decode_bool(&dataArray[i], &bValue);

                sprintf(param, "%d", (bValue == true) ? 1 : 0);

                if(prv_set_zcfg_value(omaID, instanceId, -1, LWM2M_TYPE_BOOLEAN, param) == ZCFG_SUCCESS) // FIXME:dataArray[i].type  is LWM2M_TYPE_OPAQUE but this case type should be LWM2M_TYPE_BOOLEAN, I dont know why
                {
                    result = COAP_204_CHANGED;
                }
                else
                {
                    result = COAP_400_BAD_REQUEST;
                }

                break;
            }

            case LWM2M_SECURITY_SMS_SECURITY_ID:
			{
                lwm2m_data_decode_int(&dataArray[i], &iValue);

                sprintf(param, "%d", iValue);

                if(prv_set_zcfg_value(omaID, instanceId, -1, LWM2M_TYPE_INTEGER, param) == ZCFG_SUCCESS) // FIXME:dataArray[i].type  is LWM2M_TYPE_OPAQUE,but this case type should be LWM2M_TYPE_INTEGER, I dont know why
                {
                    result = COAP_204_CHANGED;
                }
                else
                {
                    result = COAP_400_BAD_REQUEST;
                }

                break;
            }

            case LWM2M_SECURITY_SMS_KEY_PARAM_ID:
            case LWM2M_SECURITY_SMS_SECRET_KEY_ID:
            case LWM2M_SECURITY_SMS_SERVER_NUMBER_ID:
			{
                strncpy(param, dataArray[i].value.asBuffer.buffer, dataArray[i].value.asBuffer.length);


                if(prv_set_zcfg_value(omaID, instanceId, -1, dataArray[i].type, param) == ZCFG_SUCCESS)
                {
                    result = COAP_204_CHANGED;
                }
                else
                {
                    result = COAP_400_BAD_REQUEST;
                }

                break;
            }

            default:
                result = COAP_405_METHOD_NOT_ALLOWED;
                zcfgLogPrefix(ZCFG_LOG_ERR, ZCFG_LOGPRE_LWM2M, "write id=%d, default result==COAP_405_METHOD_NOT_ALLOWED\n", dataArray[i].id);
        }
#endif

        i++;
    } while (i < numData && result == COAP_204_CHANGED);

	zcfgLogPrefix(ZCFG_LOG_INFO, ZCFG_LOGPRE_LWM2M, "Write Result %d.\n", result);
	printf("%s() Jessie final result=%d\n", __FUNCTION__, result);
    return result;
}
#if 1//Jessie problem222
uint8_t prv_security_delete(lwm2m_context_t *contextP,
                                   uint16_t instanceId,
                                   lwm2m_object_t * objectP)
#else								   
static uint8_t prv_security_delete(lwm2m_context_t *contextP,
                                   uint16_t instanceId,
                                   lwm2m_object_t * objectP)
#endif								   
{
	security_instance_t * secInstance;
    char omaID[16] = {0};

    /* unused parameter */
    (void)contextP;
	#if 1//Jessie problem222
	printf("%s() Jessie Enter instanceId %d\n", __FUNCTION__, instanceId);
	#endif
    objectP->instanceList = lwm2m_list_remove(objectP->instanceList, instanceId, &secInstance);
    if (NULL == secInstance) return COAP_404_NOT_FOUND;
	#if 1//Jessie problem222
	printf("%s() Jessie secInstance found != NULL\n", __FUNCTION__);
	#endif
	if (NULL != secInstance->uri)
    {
        lwm2m_free(secInstance->uri);
    }

    lwm2m_free(secInstance);

    snprintf(omaID, 16, "%d", objectP->objID);
printf("%s() Jessie omaID=%s,instanceId=%d\n", __FUNCTION__, omaID, instanceId);
    prv_del_zcfg_value(omaID, instanceId);
	#if 1//Jessie problem222
	printf("%s() Jessie Leave\n", __FUNCTION__);
	#endif
    return COAP_202_DELETED;
}

static uint8_t prv_security_create(lwm2m_context_t *contextP,
                                   uint16_t instanceId,
                                   int numData,
                                   lwm2m_data_t * dataArray,
                                   lwm2m_object_t * objectP)
{
	#if 1
	
    security_instance_t * targetP;
    uint8_t result;
    char omaID[16] = {0};
	
    targetP = (security_instance_t *)lwm2m_malloc(sizeof(security_instance_t));
    if (NULL == targetP) return COAP_500_INTERNAL_SERVER_ERROR;
    memset(targetP, 0, sizeof(security_instance_t));

    targetP->instanceId = instanceId;
    objectP->instanceList = LWM2M_LIST_ADD(objectP->instanceList, targetP);

    snprintf(omaID, 16, "%d", objectP->objID);
    prv_add_zcfg_object(omaID, instanceId);

	result = prv_security_write(contextP, instanceId, numData, dataArray, objectP, LWM2M_WRITE_REPLACE_RESOURCES);

    if (result != COAP_204_CHANGED)
    {
        (void)prv_security_delete(contextP, instanceId, objectP);
    }
    else
    {
        result = COAP_201_CREATED;
    }

    return result;
	#else
    security_instance_t * targetP;
    uint8_t result;

    targetP = (security_instance_t *)lwm2m_malloc(sizeof(security_instance_t));
    if (NULL == targetP) return COAP_500_INTERNAL_SERVER_ERROR;
    memset(targetP, 0, sizeof(security_instance_t));

    targetP->instanceId = instanceId;
    objectP->instanceList = LWM2M_LIST_ADD(objectP->instanceList, targetP);

    result = prv_security_write(contextP, instanceId, numData, dataArray, objectP, LWM2M_WRITE_REPLACE_RESOURCES);

    if (result != COAP_204_CHANGED)
    {
        (void)prv_security_delete(contextP, instanceId, objectP);
    }
    else
    {
        result = COAP_201_CREATED;
    }

    return result;
	#endif
}
#endif

void copy_security_object(lwm2m_object_t * objectDest, lwm2m_object_t * objectSrc)
{
    memcpy(objectDest, objectSrc, sizeof(lwm2m_object_t));
    objectDest->instanceList = NULL;
    objectDest->userData = NULL;
    security_instance_t * instanceSrc = (security_instance_t *)objectSrc->instanceList;
    security_instance_t * previousInstanceDest = NULL;
    while (instanceSrc != NULL)
    {
        security_instance_t * instanceDest = (security_instance_t *)lwm2m_malloc(sizeof(security_instance_t));
        if (NULL == instanceDest)
        {
            return;
        }
        memcpy(instanceDest, instanceSrc, sizeof(security_instance_t));
        instanceDest->uri = (char*)lwm2m_malloc(strlen(instanceSrc->uri) + 1);
		if(instanceDest->uri){
			strcpy(instanceDest->uri, instanceSrc->uri);
		}
        if (instanceSrc->securityMode == LWM2M_SECURITY_MODE_PRE_SHARED_KEY)
        {
            instanceDest->publicIdentity = lwm2m_strdup(instanceSrc->publicIdentity);
            instanceDest->secretKey = lwm2m_strdup(instanceSrc->secretKey);
        }
        instanceSrc = (security_instance_t *)instanceSrc->next;
        if (previousInstanceDest == NULL)
        {
            objectDest->instanceList = (lwm2m_list_t *)instanceDest;
        }
        else
        {
            previousInstanceDest->next = instanceDest;
        }
        previousInstanceDest = instanceDest;
    }
}

void display_security_object(lwm2m_object_t * object)
{
    fprintf(stdout, "  /%u: Security object, instances:\r\n", object->objID);
    security_instance_t * instance = (security_instance_t *)object->instanceList;
    while (instance != NULL)
    {
        fprintf(stdout, "    /%u/%u: instanceId: %u, uri: %s, isBootstrap: %s, shortId: %u, clientHoldOffTime: %u\r\n",
                object->objID, instance->instanceId,
                instance->instanceId, instance->uri, instance->isBootstrap ? "true" : "false",
                instance->shortID, instance->clientHoldOffTime);
        instance = (security_instance_t *)instance->next;
    }
}

void clean_security_object(lwm2m_object_t * objectP)
{
    while (objectP->instanceList != NULL)
    {
        security_instance_t * securityInstance = (security_instance_t *)objectP->instanceList;
        objectP->instanceList = objectP->instanceList->next;
        if (NULL != securityInstance->uri)
        {
            lwm2m_free(securityInstance->uri);
        }
        if (securityInstance->securityMode == LWM2M_SECURITY_MODE_PRE_SHARED_KEY)
        {
            lwm2m_free(securityInstance->publicIdentity);
            lwm2m_free(securityInstance->secretKey);
        }
        lwm2m_free(securityInstance);
    }
}

lwm2m_object_t * get_security_object(int serverId,
                                     const char* serverUri,
                                     char * bsPskId,
                                     char * psk,
                                     uint16_t pskLen,
                                     bool isBootstrap)
{
    lwm2m_object_t * securityObj;
    char omaID[16] = {0};
    int newCount = 0;
    objIndex_t objIid;
    rdm_Lwm2mSecurity_t *lwm2mSecurityObj = NULL;
	objIndex_t objLwm2mClientIid;
    rdm_lwm2mclient_t *lwm2mLwm2mClientObj = NULL;
    uint32_t oid;
	int instanceCount = 0;//Jessie is targetP->instanceId exposed to external interface?
	char tmpBuf[256]={0};
	char *port, *host;
#ifdef WITH_TINYDTLS	
	int secretKeyLen = 0;
	char * pskBuffer = NULL;
#endif
    securityObj = (lwm2m_object_t *)lwm2m_malloc(sizeof(lwm2m_object_t));

    if (NULL != securityObj)
    {
        memset(securityObj, 0, sizeof(lwm2m_object_t));
        securityObj->objID = LWM2M_SECURITY_OBJECT_ID;

	#if 1 // yuchih	
		IID_INIT(objLwm2mClientIid);
        if(zcfgFeObjStructGet(RDM_OIDLWM2MCLIENT, &objLwm2mClientIid, (void **)&lwm2mLwm2mClientObj) != ZCFG_SUCCESS){
			clean_security_object(securityObj);
			return NULL;
		}

        IID_INIT(objIid);
        while(zcfgFeObjStructGetNext(RDM_OID_LWM2M_SECURITY, &objIid, (void **)&lwm2mSecurityObj) == ZCFG_SUCCESS)
        {
            newCount++;
			#if 1
            printf("YuChih [%s:%d] newCount = %d\n", __func__, __LINE__, newCount);

            printf("YuChih [%s:%d] lwm2mSecurityObj->LWM2M_Server_URI_0 = %s\n", __func__, __LINE__, lwm2mSecurityObj->LWM2M_Server_URI_0);
            printf("YuChih [%s:%d] lwm2mSecurityObj->BootstrapServer_1 = %d\n", __func__, __LINE__, lwm2mSecurityObj->BootstrapServer_1);
            printf("YuChih [%s:%d] lwm2mSecurityObj->Security_Mode_2 = %d\n", __func__, __LINE__, lwm2mSecurityObj->Security_Mode_2);
            printf("YuChih [%s:%d] lwm2mSecurityObj->Public_Key_or_Identity_3 = %s\n", __func__, __LINE__, lwm2mSecurityObj->Public_Key_or_Identity_3);
            printf("YuChih [%s:%d] lwm2mSecurityObj->Secret_Key_5 = %s\n", __func__, __LINE__, lwm2mSecurityObj->Secret_Key_5);
			#endif
			
            //if (newCount > 1) {
                printf("[%s:%d] Add additional security object instances (newCount = %d)\n", __func__, __LINE__, newCount);

                security_instance_t * targetP;

				targetP = (security_instance_t *)lwm2m_malloc(sizeof(security_instance_t));
				if (NULL == targetP)
				{
					lwm2m_free(securityObj);
					return NULL;
				}

				memset(targetP, 0, sizeof(security_instance_t));
				targetP->instanceId = objIid.idx[0] - 1;//Jessie
				targetP->uri = (char*)lwm2m_malloc(strlen(lwm2mSecurityObj->LWM2M_Server_URI_0)+strlen(":65535")+1); //we need to append :5683 to uri, reference main() in lwm2mclient.c sprintf (serverUri, "coap://%s:%s", server, serverPort);
				//lwm2m_connect_server() will parse uri to get port, if there is no port information appended in uri, lwm2m_connect_server() will not connect to server
				if(targetP->uri != NULL) {
					memset(targetP->uri, 0 , strlen(lwm2mSecurityObj->LWM2M_Server_URI_0)+strlen(":65535")+1);
					snprintf(tmpBuf, sizeof(tmpBuf), "%s", lwm2mSecurityObj->LWM2M_Server_URI_0);////if(strstr(&lwm2mSecurityObj->LWM2M_Server_URI_0[7], ":"))

					// parse uri in the form "coaps://[host]:[port]"
					if (0==strncmp(tmpBuf, "coaps://", strlen("coaps://"))) {
						host = tmpBuf+strlen("coaps://");
					}
					else if (0==strncmp(tmpBuf, "coap://",  strlen("coap://"))) {
						host = tmpBuf+strlen("coap://");
					} else host = NULL;

					if ( host ){
						if (host[0] == '[')
						{
							port = strrchr(host, ':');
							if (port != NULL){
								host++;
								if (*(port - 1) == ']'){
									//port++;
									snprintf(targetP->uri, strlen(lwm2mSecurityObj->LWM2M_Server_URI_0)+strlen(":65535")+1, "%s", lwm2mSecurityObj->LWM2M_Server_URI_0);
								}
							}
						} else {
							if(strstr(host, ":")){
								snprintf(targetP->uri, strlen(lwm2mSecurityObj->LWM2M_Server_URI_0)+strlen(":65535")+1, "%s", lwm2mSecurityObj->LWM2M_Server_URI_0);
							}
						}
					}

					if( 0 == strlen(targetP->uri) ){
						if(lwm2mLwm2mClientObj->Port != 0){
							snprintf(targetP->uri, strlen(lwm2mSecurityObj->LWM2M_Server_URI_0)+strlen(":65535")+1, "%s:%d", lwm2mSecurityObj->LWM2M_Server_URI_0, lwm2mLwm2mClientObj->Port);
						} else {
							snprintf(targetP->uri, strlen(lwm2mSecurityObj->LWM2M_Server_URI_0)+strlen(":65535")+1, "%s:%d", lwm2mSecurityObj->LWM2M_Server_URI_0, LWM2M_STANDARD_PORT_STR);
						}
					}

					printf("%s() Jessie targetP->uri=%s\n", __FUNCTION__, targetP->uri);
					zcfgLogPrefix(ZCFG_LOG_INFO, ZCFG_LOGPRE_LWM2M, "uri %s", targetP->uri);
				}

				targetP->securityMode = lwm2mSecurityObj->Security_Mode_2;
				targetP->publicIdentity = strdup(lwm2mSecurityObj->Public_Key_or_Identity_3);
				targetP->publicIdLen = strlen(lwm2mSecurityObj->Public_Key_or_Identity_3);
#ifndef WITH_TINYDTLS			
				targetP->secretKey = strdup(lwm2mSecurityObj->Secret_Key_5);
				targetP->secretKeyLen = strlen(lwm2mSecurityObj->Secret_Key_5);
#endif
				targetP->isBootstrap = lwm2mSecurityObj->BootstrapServer_1;
				targetP->shortID = lwm2mSecurityObj->Short_Server_ID_10;
				targetP->clientHoldOffTime = lwm2mSecurityObj->Client_Hold_Off_Time_11;
				
//#ifdef WITH_TINYDTLS
				//Skip conversion, no need to convert it to ascii string from hex
#if 0
			secretKeyLen = 0;//reset secretKeyLen
			if (strlen(lwm2mSecurityObj->Secret_Key_5) != 0)
			{
				targetP->secretKey = strdup(lwm2mSecurityObj->Secret_Key_5);
				printf("%s() Jessie 111 targetP->secretKey=%s\n", __FUNCTION__, targetP->secretKey);
				
				secretKeyLen = strlen(targetP->secretKey) / 2;
				printf("%s() Jessie 111 secretKeyLen=%d\n", __FUNCTION__, secretKeyLen);
				pskBuffer = malloc(secretKeyLen);
				
				if (NULL == pskBuffer)
				{
					fprintf(stderr, "Failed to create PSK binary buffer\r\n");
					return -1;
				}
				printf("%s() Jessie 111 sizeof(pskBuffer)=%d\n", __FUNCTION__, sizeof(pskBuffer));
				memset(pskBuffer, 0, sizeof(pskBuffer));
				// Hex string to binary
				char *h = targetP->secretKey;
				char *b = pskBuffer;
				char xlate[] = "0123456789ABCDEF";

				for ( ; *h; h += 2, ++b)
				{
					char *l = strchr(xlate, toupper(*h));
					char *r = strchr(xlate, toupper(*(h+1)));

					if (!r || !l)
					{
						fprintf(stderr, "Failed to parse Pre-Shared-Key HEXSTRING\r\n");
						return -1;
					}

					*b = ((l - xlate) << 4) + (r - xlate);
				}
			}
			#if 1
			if(targetP->secretKey){
				free(targetP->secretKey);
				//targetP->secretKey = strdup(pskBuffer);
				targetP->secretKey = malloc(secretKeyLen);
				
				if (NULL == targetP->secretKey)
				{
					fprintf(stderr, "Failed to create PSK binary buffer\r\n");
					return -1;
				}
				memset(targetP->secretKey, 0, secretKeyLen);//added
				memcpy(targetP->secretKey, pskBuffer, secretKeyLen);
				if(targetP->secretKey){
					printf("%s() Jessie targetP->secretKey=%s\n", __FUNCTION__, targetP->secretKey);
				}
			}
			targetP->secretKeyLen = secretKeyLen;
			printf("%s() Jessie secretKeyLen=%d\n", __FUNCTION__, secretKeyLen);
			#endif
#endif				
				targetP->secretKey = strdup(lwm2mSecurityObj->Secret_Key_5);
				secretKeyLen = strlen(targetP->secretKey);
				//for debugging
				//printf("%s:%d Skip conversion key: %s.\n", __func__, __LINE__, targetP->secretKey);

				securityObj->instanceList = LWM2M_LIST_ADD(securityObj->instanceList, targetP);
            //}
			
            zcfgFeObjStructFree(lwm2mSecurityObj);
        }//end while zcfgFeObjStructGetNext RDM_OID_LWM2M_SECURITY
        printf("[%s:%d] update object %d instance to %d\n", __func__, __LINE__, securityObj->objID, newCount);
	#endif

        securityObj->readFunc = prv_security_read;
#ifdef LWM2M_BOOTSTRAP
        securityObj->writeFunc = prv_security_write;
        securityObj->createFunc = prv_security_create;
        securityObj->deleteFunc = prv_security_delete;
#endif
    }//end if (NULL != securityObj)

    return securityObj;
}

char * get_server_uri(lwm2m_object_t * objectP,
                      uint16_t secObjInstID)
{
	//printf("%s() Jessie Enter secObjInstID=%d\n", __FUNCTION__, secObjInstID);
    security_instance_t * targetP = (security_instance_t *)LWM2M_LIST_FIND(objectP->instanceList, secObjInstID);

    if (NULL != targetP)
    {
		#if 0
		printf("%s() Jessie instanceId=%d\n", __FUNCTION__, targetP->instanceId);
		printf("%s() Jessie uri=%s\n", __FUNCTION__, targetP->uri);
		printf("%s() Jessie securityMode=%d\n", __FUNCTION__, targetP->securityMode);
		printf("%s() Jessie publicIdLen=%d\n", __FUNCTION__, targetP->publicIdLen);
		printf("%s() Jessie secretKeyLen=%d\n", __FUNCTION__, targetP->secretKeyLen);
		printf("%s() Jessie isBootstrap=%d\n", __FUNCTION__, targetP->isBootstrap);
		printf("%s() Jessie shortID=%d\n", __FUNCTION__, targetP->shortID);
		printf("%s() Jessie clientHoldOffTime=%d\n", __FUNCTION__, targetP->clientHoldOffTime);
		#endif
        return lwm2m_strdup(targetP->uri);
    }
	#if 0
	printf("%s() Jessie return NULL;", __FUNCTION__);
	#endif
    return NULL;
}
