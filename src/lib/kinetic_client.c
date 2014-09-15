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
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*
*/

#include "kinetic_client.h"
#include "kinetic_types_internal.h"
#include "kinetic_pdu.h"
#include "kinetic_operation.h"
#include "kinetic_connection.h"
#include "kinetic_message.h"
#include "kinetic_pdu.h"
#include "kinetic_logger.h"
#include <stdio.h>

#if 0
static KineticStatus KineticClient_ExecuteOperation(KineticOperation* operation)
{
    KineticStatus status = KINETIC_STATUS_INVALID;

    // Send the request
    if (KineticPDU_Send(operation->request))
    {
        // Associate response with same exchange as request
        operation->response->connection = operation->request->connection;

        // Receive the response
        if (KineticPDU_Receive(operation->response))
        {
            status = KineticOperation_GetStatus(operation);
        }
    }

    return status;
}
#endif

int KineticClient_Connect(KineticSession* session)
{
    if (session == NULL)
    {
        LOG("Specified KineticSession is NULL!");
        return -1;
    }

    if (strlen(session->host) == 0)
    {
        LOG("Session host is empty!");
        return -1;
    }

    if (session->hmacKey.len < 1)
    {
        LOG("Specified HMAC key is empty!");
        return -1;
    }

    if (session->hmacKey.data == NULL)
    {
        LOG("Specified HMAC key is NULL!");
        return -1;
    }

    KineticConnection* connection = KineticConnection_NewConnection(session);
    if (connection == NULL)
    {
        LOG("Failed connecting to device (connection is NULL)!");
        return -1;
    }

    if (!KineticConnection_Connect(connection))
    {
        LOGF("Failed creating connection to %s:%d",
            session->host, session->port);
        return -1;
    }

    return 0;
}

void KineticClient_Disconnect(KineticSession* session)
{
    (void)session;
    // KineticConnection_Disconnect(connection);
    // KineticConnection_FreeConnection(connection->session);
}

/**
 * @brief Creates and initializes a Kinetic operation.
 *
 * @param connection    KineticConnection instance to associate with operation
 * @param request       KineticPDU instance to use for request
 * @param response      KineticPDU instance to use for reponse
 *
 * @return              Returns a configured operation instance
 */
KineticOperation KineticClient_CreateOperation(KineticConnection* connection,
    KineticPDU* request,
    KineticPDU* response)
{
    KineticOperation op;

    if (connection == NULL)
    {
        LOG("Specified KineticConnection is NULL!");
        assert(connection != NULL);
    }

    if (request == NULL)
    {
        LOG("Specified KineticPDU request is NULL!");
        assert(request != NULL);
    }

    if (response == NULL)
    {
        LOG("Specified KineticPDU response is NULL!");
        assert(response != NULL);
    }

    KineticPDU_Init(request, connection);
    KINETIC_PDU_INIT_WITH_MESSAGE(request, connection);
    KineticPDU_Init(response, connection);

    op.connection = connection;
    op.request = request;
    op.request->proto = &op.request->protoData.message.proto;
    op.response = response;

    return op;
}

#if 0
KineticStatus KineticClient_NoOp(KineticOperation* session)
{
    assert(operation->connection != NULL);
    assert(operation->request != NULL);
    assert(operation->response != NULL);

    // Initialize request
    KineticOperation_BuildNoop(operation);

    // Execute the operation
    return KineticClient_ExecuteOperation(operation);
}

KineticStatus KineticClient_Put(KineticOperation* operation,
    const KineticKeyValue* metadata)
{
    assert(operation->connection != NULL);
    assert(operation->request != NULL);
    assert(operation->response != NULL);
    assert(metadata != NULL);
    assert(metadata->value.data != NULL);
    assert(metadata->value.len <= PDU_VALUE_MAX_LEN);

    // Initialize request
    KineticOperation_BuildPut(operation, metadata);

    // Execute the operation
    return KineticClient_ExecuteOperation(operation);
}

KineticStatus KineticClient_Get(KineticOperation* operation,
    KineticKeyValue* metadata)
{
    assert(operation->connection != NULL);
    assert(operation->request != NULL);
    assert(operation->response != NULL);
    assert(metadata != NULL);
    assert(metadata->key.data != NULL);
    assert(metadata->key.len <= KINETIC_MAX_KEY_LEN);

    if (!metadata->metadataOnly)
    {
        if (metadata->value.data == NULL)
        {
             metadata->value = (ByteArray){
                .data = operation->response->valueBuffer,
                .len = PDU_VALUE_MAX_LEN};
        }
    }

    // Initialize request
    KineticOperation_BuildGet(operation, metadata);

    // Execute the operation
    KineticStatus status = KineticClient_ExecuteOperation(operation);

    // Update the metadata with the received value length upon success
    if (status == KINETIC_STATUS_SUCCESS)
    {
        metadata->value.len = operation->response->value.len;
    }
    else
    {
        metadata->value.len = 0;
    }

    return status;
}

KineticStatus KineticClient_Delete(KineticOperation* operation,
    KineticKeyValue* metadata)
{
    assert(operation->connection != NULL);
    assert(operation->request != NULL);
    assert(operation->response != NULL);
    assert(metadata != NULL);
    assert(metadata->key.data != NULL);
    assert(metadata->key.len > 0);

    // Initialize request
    KineticOperation_BuildDelete(operation, metadata);

    // Execute the operation
    KineticStatus status = KineticClient_ExecuteOperation(operation);

    // Zero out value length for all DELETE operations
    operation->response->value.len = 0;
    metadata->value.len = 0;

    return status;
}
#endif

