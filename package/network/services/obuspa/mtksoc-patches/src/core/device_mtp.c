/*
 *
 * Copyright (C) 2019-2021, Broadband Forum
 * Copyright (C) 2016-2021  CommScope, Inc
 * Copyright (C) 2020,  BT PLC
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/**
 * \file device_mtp.c
 *
 * Implements the Device.LocalAgent.MTP data model object
 *
 */

#include <time.h>
#include <string.h>
#include <sys/socket.h>

#include "common_defs.h"
#include "data_model.h"
#include "usp_api.h"
#include "dm_access.h"
#include "dm_trans.h"
#include "mtp_exec.h"
#include "device.h"
#include "text_utils.h"
#include "nu_macaddr.h"
#include "stomp.h"

#ifdef ENABLE_COAP
#include "usp_coap.h"
#endif

//------------------------------------------------------------------------------
// Location of the local agent MTP table within the data model
#define DEVICE_AGENT_MTP_ROOT "Device.LocalAgent.MTP"
static const char device_agent_mtp_root[] = DEVICE_AGENT_MTP_ROOT;

//------------------------------------------------------------------------------
// Structure representing entries in the Device.LocalAgent.MTP.{i} table
typedef struct
{
    int instance;      // instance of the MTP in the Device.LocalAgent.MTP.{i} table
    bool enable;
    mtp_protocol_t protocol;

    // NOTE: The following parameters are not a union because the data model allows us to setup both STOMP and CoAP params at the same time, and just select between them using the protocol parameter
#ifndef DISABLE_STOMP
    int stomp_connection_instance; // Instance number of the STOMP connection which this MTP refers to (ie Device.STOMP.Connection.{i})
    char *stomp_agent_queue;    // name of the queue on the above STOMP connection, on which this agent listens
#endif

#ifdef ENABLE_COAP
    coap_config_t  coap;     // Configuration settings for CoAP server
#endif

#ifdef ENABLE_MQTT
    int mqtt_connection_instance; // Instance number of the MQTT connection which this MTP refers to (ie Device.MQTT.Client.{i})
    char *mqtt_agent_topic;    // name of the queue on the above MQTT connection, on which this agent listens
    mqtt_qos_t mqtt_publish_qos;
#endif
} agent_mtp_t;

// Array of agent MTPs
static agent_mtp_t agent_mtps[MAX_AGENT_MTPS];

//------------------------------------------------------------------------------
// Table used to convert from a textual representation of an MTP protocol to an enumeration
const enum_entry_t mtp_protocols[kMtpProtocol_Max] =
{
    { kMtpProtocol_None, "" },
#ifndef DISABLE_STOMP
    { kMtpProtocol_STOMP, "STOMP" },
#endif
#ifdef ENABLE_COAP
    { kMtpProtocol_CoAP, "CoAP" },
#endif
#ifdef ENABLE_MQTT
    { kMtpProtocol_MQTT, "MQTT" },
#endif
};

//------------------------------------------------------------------------------
// Table used to convert from an enumeration of an MTP status to a textual representation
const enum_entry_t mtp_statuses[] =
{
    { kMtpStatus_Error,  "Error" },
    { kMtpStatus_Down,   "Down" },
    { kMtpStatus_Up,     "Up" },
};

//------------------------------------------------------------------------------
// Forward declarations. Note these are not static, because we need them in the symbol table for USP_LOG_Callstack() to show them
int ValidateAdd_AgentMtp(dm_req_t *req);
int Notify_AgentMtpAdded(dm_req_t *req);
int Notify_AgentMtpDeleted(dm_req_t *req);
int Validate_AgentMtpProtocol(dm_req_t *req, char *value);
int NotifyChange_AgentMtpEnable(dm_req_t *req, char *value);
int NotifyChange_AgentMtpProtocol(dm_req_t *req, char *value);
int Get_MtpStatus(dm_req_t *req, char *buf, int len);
int ProcessAgentMtpAdded(int instance);
agent_mtp_t *FindUnusedAgentMtp(void);
void DestroyAgentMtp(agent_mtp_t *mtp);
agent_mtp_t *FindAgentMtpByInstance(int instance);

#ifndef DISABLE_STOMP
int Validate_AgentMtpStompReference(dm_req_t *req, char *value);
int Validate_AgentMtpStompDestination(dm_req_t *req, char *value);
int NotifyChange_AgentMtpStompReference(dm_req_t *req, char *value);
int NotifyChange_AgentMtpStompDestination(dm_req_t *req, char *value);
int Get_StompDestFromServer(dm_req_t *req, char *buf, int len);
#endif

#ifdef ENABLE_MQTT
//MQTT
int DEVICE_MTP_ValidateMqttReference(dm_req_t *req, char *value);
int Validate_AgentMtpProtocol(dm_req_t *req, char *value);
int NotifyChange_AgentMtpProtocol(dm_req_t *req, char *value);
int NotifyChange_AgentMtpMqtt_ResponseTopicConfigured(dm_req_t *req, char *value);
int NotifyChange_AgentMtpMqttReference(dm_req_t *req, char *value);
int Validate_AgentMtpMQTTPublishQoS(dm_req_t *req, char *value);
int NotifyChange_AgentMtpMQTTPublishQoS(dm_req_t *req, char *value);
#endif

#ifdef ENABLE_COAP
//------------------------------------------------------------------------------
// Typedef used to call either COAP_SERVER_Start() or COAP_SERVER_Stop()
typedef int (*control_coapserver_t)(int instance, char *interface, coap_config_t *config);

int NotifyChange_AgentMtpCoAPPort(dm_req_t *req, char *value);
int NotifyChange_AgentMtpCoAPPath(dm_req_t *req, char *value);
int NotifyChange_AgentMtpCoAPEncryption(dm_req_t *req, char *value);
int ControlCoapServer(agent_mtp_t *mtp, control_coapserver_t control_coapserver);
#endif

/*********************************************************************//**
**
** DEVICE_MTP_Init
**
** Initialises this component, and registers all parameters which it implements
**
** \param   None
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int DEVICE_MTP_Init(void)
{
    int err = USP_ERR_OK;
    int i;
    agent_mtp_t *mtp;

    // Mark all agent mtp slots as unused
    memset(agent_mtps, 0, sizeof(agent_mtps));
    for (i=0; i<MAX_AGENT_MTPS; i++)
    {
        mtp = &agent_mtps[i];
        mtp->instance = INVALID;
    }
#if 1//Jessie registering object and parameters with vendor_parse_tr181_and_register()
    // Register parameters implemented by this component
    err |= USP_REGISTER_Object(DEVICE_AGENT_MTP_ROOT ".{i}", ValidateAdd_AgentMtp, NULL, Notify_AgentMtpAdded,
                                                             NULL, NULL, Notify_AgentMtpDeleted);
    err |= USP_REGISTER_Param_NumEntries("Device.LocalAgent.MTPNumberOfEntries", DEVICE_AGENT_MTP_ROOT ".{i}");
    err |= USP_REGISTER_DBParam_Alias(DEVICE_AGENT_MTP_ROOT ".{i}.Alias", NULL);

    err |= USP_REGISTER_DBParam_ReadWrite(DEVICE_AGENT_MTP_ROOT ".{i}.Protocol", "STOMP", Validate_AgentMtpProtocol, NotifyChange_AgentMtpProtocol, DM_STRING);
    err |= USP_REGISTER_DBParam_ReadWrite(DEVICE_AGENT_MTP_ROOT ".{i}.Enable", "false", NULL, NotifyChange_AgentMtpEnable, DM_BOOL);

#ifndef DISABLE_STOMP
    err |= USP_REGISTER_DBParam_ReadWrite(DEVICE_AGENT_MTP_ROOT ".{i}.STOMP.Reference", "", DEVICE_MTP_ValidateStompReference, NotifyChange_AgentMtpStompReference, DM_STRING);
    err |= USP_REGISTER_DBParam_ReadWrite(DEVICE_AGENT_MTP_ROOT ".{i}.STOMP.Destination", "", NULL, NotifyChange_AgentMtpStompDestination, DM_STRING);
    err |= USP_REGISTER_VendorParam_ReadOnly(DEVICE_AGENT_MTP_ROOT ".{i}.STOMP.DestinationFromServer", Get_StompDestFromServer, DM_STRING);
#endif

#ifdef ENABLE_COAP
    err |= USP_REGISTER_DBParam_ReadWrite(DEVICE_AGENT_MTP_ROOT ".{i}.CoAP.Port", "5683", DM_ACCESS_ValidatePort, NotifyChange_AgentMtpCoAPPort, DM_UINT);
    err |= USP_REGISTER_DBParam_ReadWrite(DEVICE_AGENT_MTP_ROOT ".{i}.CoAP.Path", "", NULL, NotifyChange_AgentMtpCoAPPath, DM_STRING);
    err |= USP_REGISTER_DBParam_ReadWrite(DEVICE_AGENT_MTP_ROOT ".{i}.CoAP.EnableEncryption", "true", NULL, NotifyChange_AgentMtpCoAPEncryption, DM_BOOL);
#endif

#ifdef ENABLE_MQTT
    err |= USP_REGISTER_DBParam_ReadWrite(DEVICE_AGENT_MTP_ROOT ".{i}.MQTT.Reference", "", DEVICE_MTP_ValidateMqttReference, NotifyChange_AgentMtpMqttReference, DM_STRING);
    err |= USP_REGISTER_DBParam_ReadWrite(DEVICE_AGENT_MTP_ROOT ".{i}.MQTT.ResponseTopicConfigured", "", NULL, NotifyChange_AgentMtpMqtt_ResponseTopicConfigured, DM_STRING);
    err |= USP_REGISTER_DBParam_ReadWrite(DEVICE_AGENT_MTP_ROOT ".{i}.MQTT.PublishQoS", "2", Validate_AgentMtpMQTTPublishQoS, NotifyChange_AgentMtpMQTTPublishQoS, DM_UINT);
#endif
    err |= USP_REGISTER_VendorParam_ReadOnly(DEVICE_AGENT_MTP_ROOT ".{i}.Status", Get_MtpStatus, DM_STRING);

    // Exit if any errors occurred
    if (err != USP_ERR_OK)
    {
        return USP_ERR_INTERNAL_ERROR;
    }
#endif//Jessie
    // If the code gets here, then registration was successful
    return USP_ERR_OK;
}

/*********************************************************************//**
**
** DEVICE_MTP_Start
**
** Initialises the agent mtp array with the values of all agent MTPs from the DB
**
** \param   None
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int DEVICE_MTP_Start(void)
{
    int i;
    int_vector_t iv;
    int instance;
    int err;
    char path[MAX_DM_PATH];
	int validMtpCount;

    // Exit if unable to get the object instance numbers present in the agent MTP table
    INT_VECTOR_Init(&iv);
    err = DATA_MODEL_GetInstances(DEVICE_AGENT_MTP_ROOT, &iv);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    // Issue a warning, if no local agent MTPs are present in database
    if (iv.num_entries == 0)
    {
        USP_LOG_Warning("%s: WARNING: No instances in %s. USP Agent can only be accessed via CLI.", __FUNCTION__, device_agent_mtp_root);
        err = USP_ERR_OK;
        goto exit;
    }

	validMtpCount = 0;
    // Add all agent MTPs to the agent mtp array
    for (i=0; i < iv.num_entries; i++)
    {
        instance = iv.vector[i];
        err = ProcessAgentMtpAdded(instance);
        if (err != USP_ERR_OK)
        {
            // Exit if unable to delete an agent MTP with bad parameters from the DB
            USP_SNPRINTF(path, sizeof(path), "%s.%d", device_agent_mtp_root, instance);
            #if 1//beTr369MTPConfigLoad will check MTP, if the invalid MTP is deleted here, beTr369MTPConfigLoad will use null newMtpObj pointer.
            USP_LOG_Warning("%s: %s contained invalid parameters.", __FUNCTION__, path);
            #else
            USP_LOG_Warning("%s: Deleting %s as it contained invalid parameters.", __FUNCTION__, path);
            err = DATA_MODEL_DeleteInstance(path, 0);
            if (err != USP_ERR_OK)
            {
                goto exit;
            }
            #endif
        } else validMtpCount++;
    }
    if(validMtpCount > 0)
        err = USP_ERR_OK;
    else
        err = USP_ERR_INVALID_VALUE;

exit:
    // Destroy the vector of instance numbers for the table
    INT_VECTOR_Destroy(&iv);
    return err;
}

/*********************************************************************//**
**
** DEVICE_MTP_Stop
**
** Frees up all memory associated with this module
**
** \param   None
**
** \return  None
**
**************************************************************************/
void DEVICE_MTP_Stop(void)
{
    int i;
    agent_mtp_t *mtp;

    // Iterate over all agent MTPs, freeing all memory used by them
    for (i=0; i<MAX_AGENT_MTPS; i++)
    {
        mtp = &agent_mtps[i];
        if (mtp->instance != INVALID)
        {
            DestroyAgentMtp(mtp);
        }
    }
}

#ifndef DISABLE_STOMP
/******************************************************************//**
**
** DEVICE_MTP_GetAgentStompQueue
**
** Gets the name of the STOMP queue to use for this agent on a particular STOMP connection
**
** \param   instance - instance number of STOMP Connection in the Device.STOMP.Connection.{i} table
**
** \return  pointer to queue name, or NULL if unable to resolve the STOMP connection
**          NOTE: This may be NULL, if agent's STOMP queue is set by subscribe_dest: STOMP header
**
**************************************************************************/
char *DEVICE_MTP_GetAgentStompQueue(int instance)
{
    int i;
    agent_mtp_t *mtp;

    // Iterate over all agent MTPs, finding the first one that matches the specified STOMP connection
    // NOTE: Ideally we would have ensured that the agent_queue_name was unique for the stomp_connection_instance
    //       However it is hard to make this work in real life because when performing an ADD request, this code does
    //       not have visibility of the other parameters being performed in the add transaction, and hence cannot
    //       check the combination of agent_queue_name and stomp_connection_instance
    for (i=0; i<MAX_AGENT_MTPS; i++)
    {
        mtp = &agent_mtps[i];
        if ((mtp->instance != INVALID) && (mtp->enable == true) &&
            (mtp->stomp_connection_instance == instance) && (mtp->protocol == kMtpProtocol_STOMP) &&
            (mtp->stomp_agent_queue[0] != '\0'))
        {
            return mtp->stomp_agent_queue;
        }
    }

    // If the code gets here, then no match has been found
    return NULL;
}
#endif

/******************************************************************//**
**
** DEVICE_MTP_EnumToString
**
** Convenience function to convert an MTP enumeration to its equivalent string
**
** \param   protocol - enumerated value to convert
**
** \return  pointer to string
**
**************************************************************************/
char *DEVICE_MTP_EnumToString(mtp_protocol_t protocol)
{
    return TEXT_UTILS_EnumToString(protocol, mtp_protocols, NUM_ELEM(mtp_protocols));
}

#ifndef DISABLE_STOMP
/*********************************************************************//**
**
** DEVICE_MTP_ValidateStompReference
**
** Validates Device.LocalAgent.Controller.{i}.MTP.{i}.STOMP.Reference
** and       Device.LocalAgent.MTP.{i}.STOMP.Reference
** by checking that it refers to a valid reference in the Device.STOMP.Connection table
**
** \param   req - pointer to structure identifying the parameter
** \param   value - value that the controller would like to set the parameter to
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int DEVICE_MTP_ValidateStompReference(dm_req_t *req, char *value)
{
    int err;
    int stomp_connection_instance;

    // Exit if the STOMP Reference refers to nothing. This can occur if a STOMP connection being referred to is deleted.
    if (*value == '\0')
    {
        return USP_ERR_OK;
    }

    err = DM_ACCESS_ValidateReference(value, "Device.STOMP.Connection.{i}", &stomp_connection_instance);

    return err;
}

/*********************************************************************//**
**
** DEVICE_MTP_GetStompReference
**
** Gets the instance number in the STOMP connection table by dereferencing the specified path
** NOTE: If the path is invalid, or the instance does not exist, then INVALID is
**       returned for the instance number, along with an error
**
** \param   path - path of parameter which contains the reference
** \param   stomp_connection_instance - pointer to variable in which to return the instance number in the STOMP connection table
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int DEVICE_MTP_GetStompReference(char *path, int *stomp_connection_instance)
{
    int err;
    char value[MAX_DM_PATH];

    // Set default return value
    *stomp_connection_instance = INVALID;

    // Exit if unable to get the reference to the entry in the STOMP connection table
    // NOTE: This will return the default of an empty string if not present in the DB
    err = DATA_MODEL_GetParameterValue(path, value, sizeof(value), 0);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    // Exit if the reference has not been setup yet
    if (*value == '\0')
    {
        *stomp_connection_instance = INVALID;
        return USP_ERR_OK;
    }

    // Exit if unable to determine STOMP connection table reference
    err = DM_ACCESS_ValidateReference(value, "Device.STOMP.Connection.{i}", stomp_connection_instance);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    return USP_ERR_OK;
}


/*********************************************************************//**
**
** DEVICE_MTP_NotifyStompConnDeleted
**
** Called when a STOMP connection is deleted
** This code unpicks all references to the STOMP connection existing in the LocalAgent MTP table
**
** \param   stomp_instance - instance in Device.STOMP.Connection which has been deleted
**
** \return  None
**
**************************************************************************/
void DEVICE_MTP_NotifyStompConnDeleted(int stomp_instance)
{
    int i;
    agent_mtp_t *mtp;
    char path[MAX_DM_PATH];

    // Iterate over all agent MTPs, clearing out all references to the deleted STOMP connection
    for (i=0; i<MAX_AGENT_MTPS; i++)
    {
        mtp = &agent_mtps[i];
        if ((mtp->instance != INVALID) && (mtp->protocol == kMtpProtocol_STOMP) && (mtp->stomp_connection_instance == stomp_instance))
        {
            USP_SNPRINTF(path, sizeof(path), "Device.LocalAgent.MTP.%d.STOMP.Reference", mtp->instance);
            DATA_MODEL_SetParameterValue(path, "", 0);
        }
    }
}
#endif

/*********************************************************************//**
**
** ValidateAdd_AgentMtp
**
** Function called to determine whether an MTP may be added to an agent
**
** \param   req - pointer to structure identifying the agent MTP
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int ValidateAdd_AgentMtp(dm_req_t *req)
{
    agent_mtp_t *mtp;

    // Exit if unable to find a free MTP slot
    mtp = FindUnusedAgentMtp();
    if (mtp == NULL)
    {
        return USP_ERR_RESOURCES_EXCEEDED;
    }

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** Notify_AgentMtpAdded
**
** Function called when an MTP has been added to Device.LocalAgent.MTP.{i}
**
** \param   req - pointer to structure identifying the controller
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int Notify_AgentMtpAdded(dm_req_t *req)
{
    int err;

    err = ProcessAgentMtpAdded(inst1);

    return err;
}

/*********************************************************************//**
**
** Notify_AgentMtpDeleted
**
** Function called when an MTP has been deleted from Device.LocalAgent.MTP.{i}
**
** \param   req - pointer to structure identifying the controller
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int Notify_AgentMtpDeleted(dm_req_t *req)
{
    agent_mtp_t *mtp;

    // Exit if unable to find Agent MTP in the array
    // NOTE: We might not find it if it was never added. This could occur if deleting from the DB at startup when we detected that the database params were invalid
    mtp = FindAgentMtpByInstance(inst1);
    if (mtp == NULL)
    {
        return USP_ERR_OK;
    }

    // Exit if this MTP is not currently enbled (nothing more to do)
    if (mtp->enable == false)
    {
        return USP_ERR_OK;
    }

    // If the code gets here, then we are deleting an enabled MTP, so first turn off the protocol being used
    switch(mtp->protocol)
    {
#ifndef DISABLE_STOMP
        case kMtpProtocol_STOMP:
            // Schedule a reconnect after the present response has been sent
            if (mtp->stomp_connection_instance != INVALID)
            {
                DEVICE_STOMP_ScheduleReconnect(mtp->stomp_connection_instance);
            }
            break;
#endif

#ifdef ENABLE_COAP
        case kMtpProtocol_CoAP:
            ControlCoapServer(mtp, COAP_SERVER_Stop);
            break;
#endif

#ifdef ENABLE_MQTT
        case kMtpProtocol_MQTT:
            // Schedule a reconnect after the present response has been sent
            if (mtp->mqtt_connection_instance != INVALID)
            {
                DEVICE_MQTT_ScheduleReconnect(mtp->mqtt_connection_instance);
            }
            break;
#endif
        default:
            break;
    }

    // Delete the agent mtp from the array, if it has not already been deleted
    DestroyAgentMtp(mtp);

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** Validate_AgentMtpProtocol
**
** Validates Device.LocalAgent.MTP.{i}.Protocol
** by checking that it matches the protocols we support
**
** \param   req - pointer to structure identifying the parameter
** \param   value - value that the controller would like to set the parameter to
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int Validate_AgentMtpProtocol(dm_req_t *req, char *value)
{
    int protocol;

    // Exit if the protocol was invalid
    protocol = TEXT_UTILS_StringToEnum(value, mtp_protocols, NUM_ELEM(mtp_protocols));
    if (protocol == INVALID)
    {
        USP_ERR_SetMessage("%s: Invalid protocol %s", __FUNCTION__, value);
        return USP_ERR_INVALID_VALUE;
    }

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** NotifyChange_AgentMtpEnable
**
** Function called when Device.LocalAgent.MTP.{i}.Enable is modified
** This function updates the value of the enable stored in the agent_mtp array
**
** \param   req - pointer to structure identifying the path
** \param   value - new value of this parameter
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int NotifyChange_AgentMtpEnable(dm_req_t *req, char *value)
{
    agent_mtp_t *mtp;

    // Determine MTP to be updated
    mtp = FindAgentMtpByInstance(inst1);
    USP_ASSERT(mtp != NULL);

    // Exit if the value has not changed
    if (val_bool == mtp->enable)
    {
        return USP_ERR_OK;
    }

    // Update the protocol based on the change
    switch(mtp->protocol)
    {
#ifndef DISABLE_STOMP
        case kMtpProtocol_STOMP:
            // Store the new value
            mtp->enable = val_bool;

            // Always schedule a reconnect for the affected STOMP connection instance
            // If this MTP has been disabled, then the reconnect will fail unless another MTP specifies the agent queue to subscribe to
            if (mtp->stomp_connection_instance != INVALID)
            {
                DEVICE_STOMP_ScheduleReconnect(mtp->stomp_connection_instance);
            }
            break;
#endif

#ifdef ENABLE_COAP
        case kMtpProtocol_CoAP:
{
            // Enable or disable the CoAP server based on the new value
            int err;
            if (val_bool)
            {
                // CoAP Server has been enabled
                mtp->enable = val_bool;
                err = ControlCoapServer(mtp, COAP_SERVER_Start);
            }
            else
            {
                // CoAP Server has been disabled
                err = ControlCoapServer(mtp, COAP_SERVER_Stop);
                mtp->enable = val_bool;
            }

            // Exit if an error occurred when starting or stopping the CoAPserver
            if (err != USP_ERR_OK)
            {
                return err;
            }
}
            break;
#endif
#ifdef ENABLE_MQTT
        case kMtpProtocol_MQTT:
            // Store the new value
            mtp->enable = val_bool;

            // Always schedule a reconnect for the affected MQTT connection instance
            // If this MTP has been disabled, then the reconnect will fail unless another MTP specifies the agent queue to subscribe to
            if (mtp->mqtt_connection_instance != INVALID)
            {
                DEVICE_MQTT_ScheduleReconnect(mtp->mqtt_connection_instance);
            }
            break;
#endif
        default:
            TERMINATE_BAD_CASE(mtp->protocol);
            break;
    }

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** NotifyChange_AgentMtpProtocol
**
** Function called when Device.LocalAgent.MTP.{i}.Protocol is modified
**
** \param   req - pointer to structure identifying the path
** \param   value - new value of this parameter
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int NotifyChange_AgentMtpProtocol(dm_req_t *req, char *value)
{
    agent_mtp_t *mtp;
    mtp_protocol_t new_protocol;
    int index;

    // Determine MTP to be updated
    mtp = FindAgentMtpByInstance(inst1);
    USP_ASSERT(mtp != NULL);

    // Calculate the new ptotocol
    index = TEXT_UTILS_StringToEnum(value, mtp_protocols, NUM_ELEM(mtp_protocols));
    USP_ASSERT(index != INVALID);    // The enumeration has already been validated
    new_protocol = (mtp_protocol_t) index;

    // Exit if the value has not changed
    if (mtp->protocol == new_protocol)
    {
        return USP_ERR_OK;
    }

    // Exit if this MTP is not enabled, nothing more to do, other than cache the changed value
    if (mtp->enable == false)
    {
        mtp->protocol = new_protocol;
        return USP_ERR_OK;
    }

    // If the code gets here, then the protocol has changed

#ifdef ENABLE_COAP
    // If the existing protocol is CoAP, stop its server
    if (mtp->protocol == kMtpProtocol_CoAP)
    {
        int err = ControlCoapServer(mtp, COAP_SERVER_Stop);
        if (err != USP_ERR_OK)
        {
            return err;
        }
    }
#endif

#ifdef ENABLE_MQTT
    // Schedule the affected MQTT connection to reconnect (because it might have lost or gained a agent queue to subscribe to)
    if ((mtp->enable) && (mtp->mqtt_connection_instance != INVALID))
    {
        DEVICE_MQTT_ScheduleReconnect(mtp->mqtt_connection_instance);
    }
#endif

    // Cache the changed value
    mtp->protocol = new_protocol;

#ifndef DISABLE_STOMP
    // Schedule the affected STOMP connection to reconnect (because it might have lost or gained a agent queue to subscribe to)
    if ((mtp->enable) && (mtp->stomp_connection_instance != INVALID))
    {
        DEVICE_STOMP_ScheduleReconnect(mtp->stomp_connection_instance);
    }
#endif

#ifdef ENABLE_COAP
    // If the new protocol is CoAP, start its server
    if (new_protocol == kMtpProtocol_CoAP)
    {
        int err = ControlCoapServer(mtp, COAP_SERVER_Start);
        if (err != USP_ERR_OK)
        {
            return err;
        }
    }
#endif

    return USP_ERR_OK;
}

#ifdef ENABLE_COAP
/*********************************************************************//**
**
** NotifyChange_AgentMtpCoAPPort
**
** Function called when Device.LocalAgent.MTP.{i}.CoAP.Port is modified
**
** \param   req - pointer to structure identifying the path
** \param   value - new value of this parameter
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int NotifyChange_AgentMtpCoAPPort(dm_req_t *req, char *value)
{
    agent_mtp_t *mtp;
    int err;

    // Find the mtp server associated with this change
    mtp = FindAgentMtpByInstance(inst1);
    USP_ASSERT(mtp != NULL);

    // Exit if port has not changed
    if (val_uint == mtp->coap.port)
    {
        return USP_ERR_OK;
    }

    // Exit if failed to stop the existing CoAP server
    err = ControlCoapServer(mtp, COAP_SERVER_Stop);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    // Store the new port
    mtp->coap.port = val_uint;

    // Exit if failed to start the CoAP server on the new port
    err = ControlCoapServer(mtp, COAP_SERVER_Start);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** NotifyChange_AgentMtpCoAPPath
**
** Function called when Device.LocalAgent.MTP.{i}.CoAP.Path is modified
**
** \param   req - pointer to structure identifying the path
** \param   value - new value of this parameter
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int NotifyChange_AgentMtpCoAPPath(dm_req_t *req, char *value)
{
    int err;
    agent_mtp_t *mtp;

    // Find the mtp server associated with this change
    mtp = FindAgentMtpByInstance(inst1);
    USP_ASSERT(mtp != NULL);

    // Exit if resource has not changed
    if (strcmp(value, mtp->coap.resource)==0)
    {
        return USP_ERR_OK;
    }

    // Exit if failed to stop the existing CoAP server
    err = ControlCoapServer(mtp, COAP_SERVER_Stop);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    // Store the changed resource
    USP_SAFE_FREE(mtp->coap.resource);
    mtp->coap.resource = USP_STRDUP(value);

    // Exit if failed to start the CoAP server with the new resource
    err = ControlCoapServer(mtp, COAP_SERVER_Start);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** NotifyChange_AgentMtpCoAPEncryption
**
** Function called when Device.LocalAgent.MTP.{i}.CoAP.EnableEncryption is modified
**
** \param   req - pointer to structure identifying the path
** \param   value - new value of this parameter
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int NotifyChange_AgentMtpCoAPEncryption(dm_req_t *req, char *value)
{
    int err;
    agent_mtp_t *mtp;

    // Find the mtp server associated with this change
    mtp = FindAgentMtpByInstance(inst1);
    USP_ASSERT(mtp != NULL);

    // Exit if encryption has not changed
    if (val_bool == mtp->coap.enable_encryption)
    {
        return USP_ERR_OK;
    }

    // Exit if failed to stop the existing CoAP server
    err = ControlCoapServer(mtp, COAP_SERVER_Stop);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    // Store the changed encryption status
    mtp->coap.enable_encryption = val_bool;

    // Exit if failed to start the CoAP server with the new encryption
    err = ControlCoapServer(mtp, COAP_SERVER_Start);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    return USP_ERR_OK;
}


/*********************************************************************//**
**
** ControlCoapServer
**
** Starts or stops the specified CoAP server on all specified network interfaces
**
** \param   mtp - pointer to structure containing the CoAP server config settings
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int ControlCoapServer(agent_mtp_t *mtp, control_coapserver_t control_coapserver)
{
    int i;
    int err;
    str_vector_t sv;
    char *interfaces_to_use = COAP_LISTEN_INTERFACES;

    // Exit if this MTP is not configured to use CoAP or is disabled
    // NOTE: It is still possible to change the CoAP parameters in the data model, even if the MTP is switched to STOMP
    if ((mtp->protocol != kMtpProtocol_CoAP) || (mtp->enable == false))
    {
        return USP_ERR_OK;
    }

    // Override the compile time interface with the one specified by the '-i' command line option (if present)
    if (usp_interface != NULL)
    {
        interfaces_to_use = usp_interface;
    }

    // Split the comma separated list into each individual network interface
    TEXT_UTILS_SplitString(interfaces_to_use, &sv, ",");

    // Exit if there are not enough coap server slots for the number of network interfaces required
    if (sv.num_entries > MAX_COAP_SERVERS)
    {
        USP_ERR_SetMessage("%s: CoAP servers required on more network interfaces (%s) than slots (%d)", __FUNCTION__, interfaces_to_use, MAX_COAP_SERVERS);
        err = USP_ERR_RESOURCES_EXCEEDED;
        goto exit;
    }

    // If the list was blank, then listen on all interfaces
    if (sv.num_entries == 0)
    {
        STR_VECTOR_Add(&sv, "any");
    }

    // Start or Stop CoAP servers on all specified network interfaces
    for (i=0; i < sv.num_entries; i++)
    {
        err = control_coapserver(mtp->instance, sv.vector[i], &mtp->coap);
        if (err != USP_ERR_OK)
        {
            goto exit;
        }
    }

    // If the code gets here, then all servers were started successfully
    err = USP_ERR_OK;

exit:
    STR_VECTOR_Destroy(&sv);
    return err;
}


#endif // ENABLE_COAP

#ifndef DISABLE_STOMP
/*********************************************************************//**
**
** NotifyChange_AgentMtpStompReference
**
** Function called when Device.LocalAgent.MTP.{i}.STOMP.Reference is modified
**
** \param   req - pointer to structure identifying the path
** \param   value - new value of this parameter
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int NotifyChange_AgentMtpStompReference(dm_req_t *req, char *value)
{
    int err;
    agent_mtp_t *mtp;
    char path[MAX_DM_PATH];
    int last_connection_instance;
    int new_connection_instance;

    // Determine MTP to be updated
    mtp = FindAgentMtpByInstance(inst1);
    USP_ASSERT(mtp != NULL);

    // Exit if unable to extract the new value
    new_connection_instance = INVALID;
    USP_SNPRINTF(path, sizeof(path), "%s.%d.STOMP.Reference", device_agent_mtp_root, inst1);
    err = DEVICE_MTP_GetStompReference(path, &new_connection_instance);
    if (err != USP_ERR_OK)
    {
        mtp->stomp_connection_instance = INVALID;
        return err;
    }

    // Set the new value. This is done before scheduling a reconnect so that the reconnect uses these parameters
    last_connection_instance = mtp->stomp_connection_instance;
    mtp->stomp_connection_instance = new_connection_instance;

    // Schedule a reconnect after the present response has been sent, if the value has changed
    if ((mtp->enable == true) && (mtp->protocol == kMtpProtocol_STOMP) &&
        (last_connection_instance != new_connection_instance))
    {
        if (last_connection_instance != INVALID)
        {
            DEVICE_STOMP_ScheduleReconnect(last_connection_instance);
        }

        if (new_connection_instance != INVALID)
        {
            DEVICE_STOMP_ScheduleReconnect(new_connection_instance);
        }
    }

    return err;
}

/*********************************************************************//**
**
** NotifyChange_AgentMtpStompDestination
**
** Function called when Device.LocalAgent.MTP.{i}.STOMP.Destination is modified
**
** \param   req - pointer to structure identifying the path
** \param   value - new value of this parameter
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int NotifyChange_AgentMtpStompDestination(dm_req_t *req, char *value)
{
    agent_mtp_t *mtp;

    // Determine MTP to be updated
    mtp = FindAgentMtpByInstance(inst1);
    USP_ASSERT(mtp != NULL);

    // Determine whether to resubscribe
    if ((mtp->enable == true) && (mtp->protocol == kMtpProtocol_STOMP) &&
        (strcmp(mtp->stomp_agent_queue, value) != 0))
    {
        if (mtp->stomp_connection_instance != INVALID)
        {
            STOMP_ScheduleResubscribe(mtp->stomp_connection_instance, value);
        }
    }

    // Set the new value
    USP_SAFE_FREE(mtp->stomp_agent_queue);
    mtp->stomp_agent_queue = USP_STRDUP(value);

    return USP_ERR_OK;
}
#endif

/*********************************************************************//**
**
** Get_MtpStatus
**
** Function called to get the value of Device.LocalAgent.MTP.{i}.Status
**
** \param   req - pointer to structure identifying the path
** \param   buf - pointer to buffer in which to return the value
** \param   len - length of buffer
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int Get_MtpStatus(dm_req_t *req, char *buf, int len)
{
    agent_mtp_t *mtp;
    mtp_status_t status;
    char *str;

    // Determine which MTP to get the status of
    mtp = FindAgentMtpByInstance(inst1);
    USP_ASSERT(mtp != NULL);

    // Get the status, based on the protocol
    if (mtp->enable)
    {
        switch(mtp->protocol)
        {
#ifndef DISABLE_STOMP
            case kMtpProtocol_STOMP:
                status = DEVICE_STOMP_GetMtpStatus(mtp->stomp_connection_instance);
                break;
#endif

#ifdef ENABLE_COAP
            case kMtpProtocol_CoAP:
                status = COAP_SERVER_GetStatus(mtp->instance);
                break;
#endif

#ifdef ENABLE_MQTT
            case kMtpProtocol_MQTT:
                status = DEVICE_MQTT_GetMtpStatus(mtp->mqtt_connection_instance);
                break;
#endif

            default:
                // NOTE: The code should never get here, as we only allow valid MTPs to be set
                status = kMtpStatus_Error;
                break;
        }
    }
    else
    {
        // If not enabled, then always report that the interface is down
        status = kMtpStatus_Down;
    }

    // Convert to a string representation and copy into return buffer
    str = TEXT_UTILS_EnumToString(status, mtp_statuses, NUM_ELEM(mtp_statuses));
    USP_STRNCPY(buf, str, len);

    return USP_ERR_OK;
}

#ifndef DISABLE_STOMP
/*********************************************************************//**
**
** Get_StompDestFromServer
**
** Function called to get the value of Device.LocalAgent.MTP.{i}.DestinationFromServer
**
** \param   req - pointer to structure identifying the path
** \param   buf - pointer to buffer in which to return the value
** \param   len - length of buffer
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int Get_StompDestFromServer(dm_req_t *req, char *buf, int len)
{
    agent_mtp_t *mtp;

    // Set the default return value
    *buf = '\0';

    // Determine which MTP to query
    mtp = FindAgentMtpByInstance(inst1);
    USP_ASSERT(mtp != NULL);

    // Get the DestinationFromServer
    if ((mtp->enable) && (mtp->protocol==kMtpProtocol_STOMP))
    {
        DEVICE_STOMP_GetDestinationFromServer(mtp->stomp_connection_instance, buf, len);
    }

    return USP_ERR_OK;
}
#endif

/*********************************************************************//**
**
** ProcessAgentMtpAdded
**
** Reads the parameters for the specified MTP from the database and processes them
**
** \param   instance - instance number of the MTP in the local agent MTP table
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int ProcessAgentMtpAdded(int instance)
{
    agent_mtp_t *mtp;
    int err;
    char path[MAX_DM_PATH];

    // Exit if unable to add another agent MTP
    mtp = FindUnusedAgentMtp();
    if (mtp == NULL)
    {
        return USP_ERR_RESOURCES_EXCEEDED;
    }

    // Initialise to defaults
    memset(mtp, 0, sizeof(agent_mtp_t));
    mtp->instance = instance;
#ifndef DISABLE_STOMP
    mtp->stomp_connection_instance = INVALID;
#endif
#ifdef ENABLE_MQTT
    mtp->mqtt_connection_instance = INVALID;
#endif
    mtp->instance = instance;

    // Exit if unable to determine whether this agent MTP was enabled or not
    USP_SNPRINTF(path, sizeof(path), "%s.%d.Enable", device_agent_mtp_root, instance);
    err = DM_ACCESS_GetBool(path, &mtp->enable);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    // Exit if unable to get the protocol for this MTP
    USP_SNPRINTF(path, sizeof(path), "%s.%d.Protocol", device_agent_mtp_root, instance);
    err = DM_ACCESS_GetEnum(path, &mtp->protocol, mtp_protocols, NUM_ELEM(mtp_protocols));
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    // NOTE: We attempt to get all parameters, irrespective of the protocol that was actually set
    // This is because the data model allows us to setup both STOMP and CoAP params at the same time, and just select between them using the protocol parameter
    // If the parameter is not present in the database, then it's default value will be read

#ifndef DISABLE_STOMP
    // Exit if there was an error in the reference to the entry in the STOMP connection table
    USP_SNPRINTF(path, sizeof(path), "%s.%d.STOMP.Reference", device_agent_mtp_root, instance);
    err = DEVICE_MTP_GetStompReference(path, &mtp->stomp_connection_instance);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    // Exit if unable to get the name of the agent's STOMP queue
    USP_SNPRINTF(path, sizeof(path), "%s.%d.STOMP.Destination", device_agent_mtp_root, instance);
    err = DM_ACCESS_GetString(path, &mtp->stomp_agent_queue);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }
#endif

#ifdef ENABLE_MQTT
    // Exit if there was an error in the reference to the entry in the MQTT client table
    USP_SNPRINTF(path, sizeof(path), "%s.%d.MQTT.Reference", device_agent_mtp_root, instance);
    err = DEVICE_MTP_GetMqttReference(path, &mtp->mqtt_connection_instance);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    // Exit if unable to get the name of the agent's MQTT topic
    USP_SNPRINTF(path, sizeof(path), "%s.%d.MQTT.ResponseTopicConfigured", device_agent_mtp_root, instance);
    err = DM_ACCESS_GetString(path, &mtp->mqtt_agent_topic);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    // Exit if there was an error in the pusblish qos to the entry in the MQTT client table
    USP_SNPRINTF(path, sizeof(path), "%s.%d.MQTT.PublishQoS", device_agent_mtp_root, instance);
    err = DM_ACCESS_GetUnsigned(path, (unsigned int*)&mtp->mqtt_publish_qos);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }
#endif

#ifdef ENABLE_COAP
    // Exit if unable to get the listening port to use for CoAP
    USP_SNPRINTF(path, sizeof(path), "%s.%d.CoAP.Port", device_agent_mtp_root, instance);
    err = DM_ACCESS_GetUnsigned(path, &mtp->coap.port);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    // Exit if unable to get the name of the agent's CoAP resource name path
    USP_SNPRINTF(path, sizeof(path), "%s.%d.CoAP.Path", device_agent_mtp_root, instance);
    err = DM_ACCESS_GetString(path, &mtp->coap.resource);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    // Exit if unable to get whether to use an encrypted CoAP connection or not
    USP_SNPRINTF(path, sizeof(path), "%s.%d.CoAP.EnableEncryption", device_agent_mtp_root, instance);
    err = DM_ACCESS_GetBool(path, &mtp->coap.enable_encryption);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    // Exit if the protocol was CoAP and unable to start the CoAP servers
    if (mtp->protocol == kMtpProtocol_CoAP)
    {
        err = ControlCoapServer(mtp, COAP_SERVER_Start);
        if (err != USP_ERR_OK)
        {
            goto exit;
        }
    }
#endif

    // If the code gets here, then we successfully retrieved all data about the MTP
    err = USP_ERR_OK;

exit:
    if (err != USP_ERR_OK)
    {
        DestroyAgentMtp(mtp);
    }

#ifndef DISABLE_STOMP
    // Schedule a STOMP reconnect, if this MTP affects an existing STOMP connection
    if ((mtp->enable) && (mtp->protocol==kMtpProtocol_STOMP) && (mtp->stomp_connection_instance != INVALID))
    {
        DEVICE_STOMP_ScheduleReconnect(mtp->stomp_connection_instance);
    }
#endif

#ifdef ENABLE_MQTT
    // Schedule a MQTT reconnect, if this MTP affects an existing MQTT client
    if ((mtp->enable) && (mtp->protocol==kMtpProtocol_MQTT) && (mtp->mqtt_connection_instance != INVALID))
    {
        DEVICE_MQTT_ScheduleReconnect(mtp->mqtt_connection_instance);
    }
#endif

    return err;
}

/*********************************************************************//**
**
** FindUnusedAgentMtp
**
** Finds the first free agent MTP slot
**
** \param   None
**
** \return  Pointer to first free agent MTP, or NULL if no free agent MTP slot found
**
**************************************************************************/
agent_mtp_t *FindUnusedAgentMtp(void)
{
    int i;
    agent_mtp_t *mtp;

    // Iterate over all agent MTPs
    for (i=0; i<MAX_AGENT_MTPS; i++)
    {
        // Exit if found an unused slot
        mtp = &agent_mtps[i];
        if (mtp->instance == INVALID)
        {
            return mtp;
        }
    }

    // If the code gets here, then no free slot has been found
    USP_ERR_SetMessage("%s: Only %d agent MTPs are supported.", __FUNCTION__, MAX_AGENT_MTPS);
    return NULL;
}

/*********************************************************************//**
**
** DestroyAgentMtp
**
** Frees all memory associated with the specified agent mtp slot
**
** \param   cont - pointer to controller to free
**
** \return  None
**
**************************************************************************/
void DestroyAgentMtp(agent_mtp_t *mtp)
{
    mtp->instance = INVALID;      // Mark agent MTP slot as free
    mtp->protocol = kMtpProtocol_None;
    mtp->enable = false;

#ifndef DISABLE_STOMP
    mtp->stomp_connection_instance = INVALID;
    USP_SAFE_FREE(mtp->stomp_agent_queue);
#endif

#ifdef ENABLE_COAP
    USP_SAFE_FREE(mtp->coap.resource);
    mtp->coap.port = 0;
#endif

#ifdef ENABLE_MQTT
    mtp->mqtt_connection_instance = INVALID;
    USP_SAFE_FREE(mtp->mqtt_agent_topic);
#endif
}

/*********************************************************************//**
**
** FindAgentMtpByInstance
**
** Finds an agent MTP entry by it's data model instance number
**
** \param   instance - instance number of the agent MTP in the data model
**
** \return  pointer to entry within the agent_mtps array, or NULL if mtp was not found
**
**************************************************************************/
agent_mtp_t *FindAgentMtpByInstance(int instance)
{
    int i;
    agent_mtp_t *mtp;

    // Iterate over all agent MTPs
    for (i=0; i<MAX_AGENT_MTPS; i++)
    {
        // Exit if found an agent mtp that matches the instance number
        mtp = &agent_mtps[i];
        if (mtp->instance == instance)
        {
            return mtp;
        }
    }

    // If the code gets here, then no matching MTP was found
    return NULL;
}
#ifdef ENABLE_MQTT
/******************************************************************//**
**
** DEVICE_MTP_GetAgentMqttResponseTopic
**
** Gets the name of the MQTT queue to use for this agent on a particular MQTT client connection
**
** \param   instance - instance number of MQTT Clients Connection in the Device.MQTT.Client.{i} table
**
** \return  pointer to queue name, or NULL if unable to resolve the MQTT connection
**
**************************************************************************/
char *DEVICE_MTP_GetAgentMqttResponseTopic(int instance)
{
    int i;
    agent_mtp_t *mtp;

    // Iterate over all agent MTPs, finding the first one that matches the specified MQTT client
    for (i=0; i<MAX_AGENT_MTPS; i++)
    {
        mtp = &agent_mtps[i];
        if ((mtp->instance != INVALID) && (mtp->enable == true) &&
            (mtp->mqtt_connection_instance == instance) && (mtp->protocol == kMtpProtocol_MQTT) &&
            (mtp->mqtt_agent_topic[0] != '\0'))
        {
            return mtp->mqtt_agent_topic;
        }
    }

    // If the code gets here, then no match has been found
    return NULL;
}

/*********************************************************************//**
**
** DEVICE_MTP_ValidateMqttReference
**
** Validates Device.LocalAgent.Controller.{i}.MTP.{i}.MQTT.Reference
** and       Device.LocalAgent.MTP.{i}.MQTT.Reference
** by checking that it refers to a valid reference in the Device.MQTT.Client table
**
** \param   req - pointer to structure identifying the parameter
** \param   value - value that the controller would like to set the parameter to
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int DEVICE_MTP_ValidateMqttReference(dm_req_t *req, char *value)
{
    int err;
    int mqtt_connection_instance;

    // Exit if the MQTT Reference refers to nothing. This can occur if a MQTT client being referred to is deleted.
    if (*value == '\0')
    {
        return USP_ERR_OK;
    }

    err = DM_ACCESS_ValidateReference(value, "Device.MQTT.Client.{i}", &mqtt_connection_instance);

    return err;
}

/*********************************************************************//**
**
** DEVICE_MTP_GetMqttReference
**
** Gets the instance number in the MQTT client table by dereferencing the specified path
** NOTE: If the path is invalid, or the instance does not exist, then INVALID is
**       returned for the instance number, along with an error
**
** \param   path - path of parameter which contains the reference
** \param   mqtt_connection_instance - pointer to variable in which to return the instance number in the MQTT client table
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int DEVICE_MTP_GetMqttReference(char *path, int *mqtt_connection_instance)
{
    int err;
    char value[MAX_DM_PATH];

    // Set default return value
    *mqtt_connection_instance = INVALID;

    // Exit if unable to get the reference to the entry in the MQTT client table
    // NOTE: This will return the default of an empty string if not present in the DB
    err = DATA_MODEL_GetParameterValue(path, value, sizeof(value), 0);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    // Exit if the reference has not been setup yet
    if (*value == '\0')
    {
        *mqtt_connection_instance = INVALID;
        return USP_ERR_OK;
    }

    // Exit if unable to determine MQTT client table reference
    err = DM_ACCESS_ValidateReference(value, "Device.MQTT.Client.{i}", mqtt_connection_instance);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** DEVICE_MTP_NotifyMqttConnDeleted
**
** Called when a MQTT client is deleted
** This code unpicks all references to the MQTT client existing in the LocalAgent MTP table
**
** \param   mqtt_instance - instance in Device.MQTT.Client which has been deleted
**
** \return  None
**
**************************************************************************/
void DEVICE_MTP_NotifyMqttConnDeleted(int mqtt_instance)
{
    int i;
    agent_mtp_t *mtp;
    char path[MAX_DM_PATH];

    // Iterate over all agent MTPs, clearing out all references to the deleted MQTT client
    for (i=0; i<MAX_AGENT_MTPS; i++)
    {
        mtp = &agent_mtps[i];
        if ((mtp->instance != INVALID) && (mtp->protocol == kMtpProtocol_MQTT) && (mtp->mqtt_connection_instance == mqtt_instance))
        {
            USP_SNPRINTF(path, sizeof(path), "Device.LocalAgent.MTP.%d.MQTT.Reference", mtp->instance);
            DATA_MODEL_SetParameterValue(path, "", 0);
        }
    }
}

/*********************************************************************//**
**
** NotifyChange_AgentMtpMqttReference
**
** Function called when Device.LocalAgent.MTP.{i}.MQTT.Reference is modified
**
** \param   req - pointer to structure identifying the path
** \param   value - new value of this parameter
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int NotifyChange_AgentMtpMqttReference(dm_req_t *req, char *value)
{
    int err;
    agent_mtp_t *mtp;
    char path[MAX_DM_PATH];
    int last_connection_instance;
    int new_connection_instance;

    // Determine MTP to be updated
    mtp = FindAgentMtpByInstance(inst1);
    USP_ASSERT(mtp != NULL);

    // Exit if unable to extract the new value
    new_connection_instance = INVALID;
    USP_SNPRINTF(path, sizeof(path), "%s.%d.MQTT.Reference", device_agent_mtp_root, inst1);
    err = DEVICE_MTP_GetMqttReference(path, &new_connection_instance);
    if (err != USP_ERR_OK)
    {
        mtp->mqtt_connection_instance = INVALID;
        return err;
    }

    // Set the new value. This is done before scheduling a reconnect so that the reconnect uses these parameters
    last_connection_instance = mtp->mqtt_connection_instance;
    mtp->mqtt_connection_instance = new_connection_instance;

    // Schedule a reconnect after the present response has been sent, if the value has changed
    if ((mtp->enable == true) && (mtp->protocol == kMtpProtocol_MQTT) &&
        (last_connection_instance != new_connection_instance))
    {
        if (last_connection_instance != INVALID)
        {
            DEVICE_MQTT_ScheduleReconnect(last_connection_instance);
        }

        if (new_connection_instance != INVALID)
        {
            DEVICE_MQTT_ScheduleReconnect(new_connection_instance);
        }
    }

    return err;
}

/*********************************************************************//**
**
** NotifyChange_AgentMtpMqtt_ResponseTopicConfigured
**
** Function called when Device.LocalAgent.MTP.{i}.MQTT.ResponseTopicConfigured is modified
**
** \param   req - pointer to structure identifying the path
** \param   value - new value of this parameter
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int NotifyChange_AgentMtpMqtt_ResponseTopicConfigured(dm_req_t *req, char *value)
{
    agent_mtp_t *mtp;
    bool schedule_reconnect = false;

    // Determine MTP to be updated
    mtp = FindAgentMtpByInstance(inst1);
    USP_ASSERT(mtp != NULL);

    // Determine whether to reconnect
    if ((mtp->enable == true) && (mtp->protocol == kMtpProtocol_MQTT) &&
        (strcmp(mtp->mqtt_agent_topic, value) != 0))
    {
        if (mtp->mqtt_connection_instance != INVALID)
        {
            schedule_reconnect = true;
        }
    }

    // Set the new value
    // This is done before scheduling a reconnect, so that the reconnect is done with the new parameters
    USP_SAFE_FREE(mtp->mqtt_agent_topic);
    mtp->mqtt_agent_topic = USP_STRDUP(value);

    if (schedule_reconnect)
    {
        DEVICE_MQTT_ScheduleReconnect(mtp->mqtt_connection_instance);
    }

    return USP_ERR_OK;
}


/*********************************************************************//**
**
** Validate_AgentMtpMQTTPublishQoS
**
** Validates Device.LocalAgent.MTP.{i}.MQTT.PublishQoS by checking if valid number
**
** \param   req - pointer to structure identifying the parameter
** \param   value - value that the controller would like to set the parameter to
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int Validate_AgentMtpMQTTPublishQoS(dm_req_t *req, char *value)
{
    return DM_ACCESS_ValidateRange_Unsigned(req, 0, 2);
}

/*********************************************************************//**
**** NotifyChange_AgentMtpMQTTPublishQoS
**
** Function called when Device.LocalAgent.MTP.{i}.MQTT.PublishQoS is modified
**
** \param   req - pointer to structure identifying the path
** \param   value - new value of this parameter
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int NotifyChange_AgentMtpMQTTPublishQoS(dm_req_t *req, char *value)
{
    agent_mtp_t *mtp;
    int last_publish_qos;
    int new_publish_qos;

    // Determine MTP to be updated
    mtp = FindAgentMtpByInstance(inst1);
    USP_ASSERT(mtp != NULL);

    // Exit if unable to extract the new value
    new_publish_qos = val_uint;

    // Set the new value. This is done before scheduling a reconnect so that the reconnect uses these parameters
    last_publish_qos = mtp->mqtt_publish_qos;
    mtp->mqtt_publish_qos = new_publish_qos;

    // Schedule a reconnect after the present response has been sent, if the value has changed
    if ((mtp->enable == true) && (mtp->protocol == kMtpProtocol_MQTT) &&
        (last_publish_qos != new_publish_qos))
    {
        DEVICE_MQTT_ScheduleReconnect(mtp->mqtt_connection_instance);
    }

    return USP_ERR_OK;
}

agent_mtp_t *FindAgentMtpMQTTReference(int reference)
{
    int i;
    agent_mtp_t *mtp;

    // Iterate over all agent MTPs
    for (i=0; i<MAX_AGENT_MTPS; i++)
    {
        // Exit if found an agent mtp that matches the instance number
        mtp = &agent_mtps[i];
        if (mtp->mqtt_connection_instance == reference)
        {
            return mtp;
        }
    }

    // If the code gets here, then no matching MTP was found
    return NULL;
}
int device_Update_LocalAgent_MTP_MQTT_Parameters(int instance, int *valueChangeHandle){
    char path[MAX_DM_PATH], *tempStr = NULL;
	int err = USP_ERR_OK, tempNum;
    agent_mtp_t *mtp = NULL;
	mqtt_conn_params_t *mp = NULL;
	bool tempBool = false;

    // Determine MTP to be updated
    mtp = FindAgentMtpByInstance(instance);
	if(mtp == NULL){
		USP_LOG_Error("%s:%d Fail to find MTP with instance %d.\n", __func__, __LINE__, instance);
		return -1;
	}

#if 0
	if(mtp->protocol != kMtpProtocol_MQTT){
		USP_LOG_Error("%s:%d Only handle MQTT instance %d.\n", __func__, __LINE__, instance);
		return -1;
	}
#endif

	printf("[Marcus debug] %s:%d inst %d, handle %d, proto %d.\n", __func__, __LINE__, instance, *valueChangeHandle, mtp->protocol);
#if 0
	mp = FindMqttParamsByInstance(instance);
	if(mp == NULL){
		USP_LOG_Error("%s:%d Fail to find MQTT with instance %d.\n", __func__, __LINE__, instance);
		return -1;
	}
#endif

    // Exit if unable to determine whether this agent MTP was enabled or not
    USP_SNPRINTF(path, sizeof(path), "%s.%d.Enable", device_agent_mtp_root, instance);
	tempBool = mtp->enable;
    err = DM_ACCESS_GetBool(path, &mtp->enable);
    if (err != USP_ERR_OK)
    {
		USP_LOG_Error("%s:%d Get path %s error(%d).\n", __func__, __LINE__, path, err);
		return err;
    }
	if(tempBool != mtp->enable){
		USP_LOG_Info("%s:%d Path %s value changed (%s to %s).\n", __func__, __LINE__, path, tempBool?"Enable":"Disable", \
																							mtp->enable?"Enable":"Disable");

		mp = NULL;
		mp = FindMqttParamsByInstance(mtp->mqtt_connection_instance);
		//find linked MQTT client and restart the connection if needed
		if(mp != NULL){
			if(mp->enable && mtp->enable){
				USP_LOG_Info("%s:%d Find MQTT reference %d.\n", __func__, __LINE__, mtp->mqtt_connection_instance);
				if(strcmp("Running", MQTT_GetClientStatus(mtp->mqtt_connection_instance)) != 0){
					printf("[Marcus debug] %s:%d VALUE_CHANGED_HANDLE_LAUNCHMQTT.\n", __func__, __LINE__);
					valueChangeHandle[mtp->mqtt_connection_instance] |= VALUE_CHANGED_HANDLE_LAUNCHMQTT;
				}
			}
			else{
				USP_LOG_Info("%s:%d Find MQTT reference %d.\n", __func__, __LINE__, mtp->mqtt_connection_instance);
				if(strcmp("Running", MQTT_GetClientStatus(mtp->mqtt_connection_instance)) == 0){
					printf("[Marcus debug] %s:%d VALUE_CHANGED_HANDLE_SHUTOFFMQTT.\n", __func__, __LINE__);
					valueChangeHandle[mtp->mqtt_connection_instance] |= VALUE_CHANGED_HANDLE_SHUTOFFMQTT;
				}
			}
		}
	}

    // Exit if there was an error in the pusblish qos to the entry in the MQTT client table
    USP_SNPRINTF(path, sizeof(path), "%s.%d.MQTT.PublishQoS", device_agent_mtp_root, instance);
	tempNum = mtp->mqtt_publish_qos;
    err = DM_ACCESS_GetUnsigned(path, (unsigned int*)&mtp->mqtt_publish_qos);
    if (err != USP_ERR_OK)
	{
		USP_LOG_Error("%s:%d Get path %s error(%d).\n", __func__, __LINE__, path, err);
		return err;
	}
	if(tempNum != mtp->mqtt_publish_qos){
		USP_LOG_Info("%s:%d Path %s value changed (%d to %d).\n", __func__, __LINE__, path, tempNum, mtp->mqtt_publish_qos);

		mp = NULL;
		mp = FindMqttParamsByInstance(mtp->mqtt_connection_instance);
		//find linked MQTT client and restart the connection if needed
		if(mp != NULL && (mp->enable)){
			USP_LOG_Info("%s:%d Find MQTT reference %d.\n", __func__, __LINE__, mtp->mqtt_connection_instance);
			valueChangeHandle[mtp->mqtt_connection_instance] |= VALUE_CHANGED_HANDLE_RECONNECTMQTT;
		}
	}

    // Exit if there was an error in the reference to the entry in the MQTT client table
    USP_SNPRINTF(path, sizeof(path), "%s.%d.MQTT.Reference", device_agent_mtp_root, instance);
	tempNum = mtp->mqtt_connection_instance;
    err = DEVICE_MTP_GetMqttReference(path, &mtp->mqtt_connection_instance);
    if (err != USP_ERR_OK)
    {
		USP_LOG_Error("%s:%d Get path %s error(%d).\n", __func__, __LINE__, path, err);
		return err;
    }
	if(tempNum != mtp->mqtt_connection_instance){
		USP_LOG_Info("%s:%d Path %s value changed (%d to %d).\n", __func__, __LINE__, path, tempNum, mtp->mqtt_connection_instance);

		mp = NULL;
		mp = FindMqttParamsByInstance(mtp->mqtt_connection_instance);
		//find linked MQTT client and restart the connection if needed
		if(mp != NULL && (mp->enable)){
			USP_LOG_Info("%s:%d Find MQTT reference %d.\n", __func__, __LINE__, mtp->mqtt_connection_instance);
			valueChangeHandle[mtp->mqtt_connection_instance] |= VALUE_CHANGED_HANDLE_RECONNECTMQTT;
		}
	}

    // Exit if unable to get the name of the agent's MQTT topic
    USP_SNPRINTF(path, sizeof(path), "%s.%d.MQTT.ResponseTopicConfigured", device_agent_mtp_root, instance);
	tempStr = mtp->mqtt_agent_topic;
    err = DM_ACCESS_GetString(path, &mtp->mqtt_agent_topic);
    if (err != USP_ERR_OK)
    {
		USP_LOG_Error("%s:%d Get path %s error(%d).\n", __func__, __LINE__, path, err);
		return err;
    }
	if(strcmp(tempStr, mtp->mqtt_agent_topic) != 0){
		USP_LOG_Info("%s:%d Path %s value changed (%s to %s).\n", __func__, __LINE__, path, tempStr, mtp->mqtt_agent_topic);
#if 0   // no action for topic changed
		mp = NULL;
		mp = FindMqttParamsByInstance(mtp->mqtt_connection_instance);
		//find linked MQTT client and restart the connection if needed
		if(mp != NULL && (mp->enable)){
			USP_LOG_Info("%s:%d Find MQTT reference %d.\n", __func__, __LINE__, mtp->mqtt_connection_instance);
			valueChangeHandle[mtp->mqtt_connection_instance] |= VALUE_CHANGED_HANDLE_RECONNECTMQTT;
		}
#endif
	}
	USP_SAFE_FREE(tempStr);
}
#endif
