/*
* kinetic-c
* Copyright (C) 2014 Seagate Technology.
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*
*/
#include "kinetic_request.h"
#include <pthread.h>

#include "kinetic_logger.h"
#include "kinetic_session.h"
#include "kinetic_auth.h"
#include "kinetic_nbo.h"
#include "kinetic_controller.h"
#include "byte_array.h"
#include "bus.h"

#ifdef TEST
uint8_t *cmdBuf = NULL;
uint8_t *msg = NULL;
#endif

size_t KineticRequest_PackCommand(KineticRequest* request)
{
    size_t expectedLen = KineticProto_command__get_packed_size(&request->message.command);
    #ifndef TEST
    uint8_t *cmdBuf = (uint8_t*)malloc(expectedLen);
    #endif
    if (cmdBuf == NULL)
    {
        LOGF0("Failed to allocate command bytes: %zd", expectedLen);
        return KINETIC_REQUEST_PACK_FAILURE;
    }
    request->message.message.commandBytes.data = cmdBuf;

    size_t packedLen = KineticProto_command__pack(
        &request->message.command, cmdBuf);
    KINETIC_ASSERT(packedLen == expectedLen);
    request->message.message.commandBytes.len = packedLen;
    request->message.message.has_commandBytes = true;
    KineticLogger_LogByteArray(3, "commandBytes", (ByteArray){
        .data = request->message.message.commandBytes.data,
        .len = request->message.message.commandBytes.len,
    });

    return packedLen;
}

KineticStatus KineticRequest_PopulateAuthentication(KineticSessionConfig *config,
    KineticRequest *request, ByteArray *pin)
{
    if (pin != NULL) {
        return KineticAuth_PopulatePin(config, request, *pin);
    } else {
        return KineticAuth_PopulateHmac(config, request);
    }
}

KineticStatus KineticRequest_PackMessage(KineticOperation *operation,
    uint8_t **out_msg, size_t *msgSize)
{
    // Configure PDU header
    KineticProto_Message* proto = &operation->request->message.message;
    KineticPDUHeader header = {
        .versionPrefix = 'F',
        .protobufLength = KineticProto_Message__get_packed_size(proto)
    };
    header.valueLength = operation->value.len;
    uint32_t nboProtoLength = KineticNBO_FromHostU32(header.protobufLength);
    uint32_t nboValueLength = KineticNBO_FromHostU32(header.valueLength);

    // Allocate and pack protobuf message
    size_t offset = 0;
    #ifndef TEST
    uint8_t *msg = malloc(PDU_HEADER_LEN + header.protobufLength + header.valueLength);
    #endif
    if (msg == NULL) {
        LOG0("Failed to allocate outgoing message!");
        return KINETIC_STATUS_MEMORY_ERROR;
    }

    // Pack header
    KineticRequest* request = operation->request;
    msg[offset] = header.versionPrefix;
    offset += sizeof(header.versionPrefix);
    memcpy(&msg[offset], &nboProtoLength, sizeof(nboProtoLength));
    offset += sizeof(nboProtoLength);
    memcpy(&msg[offset], &nboValueLength, sizeof(nboValueLength));
    offset += sizeof(nboValueLength);
    size_t len = KineticProto_Message__pack(&request->message.message, &msg[offset]);
    KINETIC_ASSERT(len == header.protobufLength);
    offset += header.protobufLength;

    #ifndef TEST
    // Log protobuf per configuration
    LOGF2("[PDU TX] pdu: %p, session: %p, bus: %p, "
        "fd: %6d, seq: %8lld, protoLen: %8u, valueLen: %8u, op: %p, msgType: %02x",
        (void*)operation->request,
        (void*)operation->connection->pSession, (void*)operation->connection->messageBus,
        operation->connection->socket, (long long)request->message.header.sequence,
        header.protobufLength, header.valueLength,
        (void*)operation, request->message.header.messageType);
    KineticLogger_LogHeader(3, &header);
    KineticLogger_LogProtobuf(3, proto);
    #endif
    
    // Pack value payload, if supplied
    if (header.valueLength > 0) {
        memcpy(&msg[offset], operation->value.data, operation->value.len);
        offset += operation->value.len;
    }
    KINETIC_ASSERT((PDU_HEADER_LEN + header.protobufLength + header.valueLength) == offset);

    *out_msg = msg;
    *msgSize = offset;
    return KINETIC_STATUS_SUCCESS;
}

bool KineticRequest_SendRequest(KineticOperation *operation,
    uint8_t *msg, size_t msgSize)
{
    KINETIC_ASSERT(msg);
    KINETIC_ASSERT(msgSize > 0);
    bus_user_msg bus_msg = {
        .fd       = operation->connection->socket,
        .type     = BUS_SOCKET_PLAIN,  // FIXME: no SSL?
        .seq_id   = operation->request->message.header.sequence,
        .msg      = msg,
        .msg_size = msgSize,
        .cb       = KineticController_HandleResult,
        .udata    = operation,
        .timeout_sec = operation->timeoutSeconds,
    };
    return bus_send_request(operation->connection->messageBus, &bus_msg);
}

bool KineticRequest_LockConnection(KineticConnection *connection)
{
    return 0 == pthread_mutex_lock(&connection->sendMutex);
}

bool KineticRequest_UnlockConnection(KineticConnection *connection)
{
    return 0 == pthread_mutex_unlock(&connection->sendMutex);
}