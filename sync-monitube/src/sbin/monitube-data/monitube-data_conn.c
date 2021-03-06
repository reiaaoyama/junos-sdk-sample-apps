/*
 * $Id: monitube-data_conn.c 346460 2009-11-14 05:06:47Z ssiano $
 *
 * This code is provided as is by Juniper Networks SDK Developer Support.
 * It is provided with no warranties or guarantees, and Juniper Networks
 * will not provide support or maintenance of this code in any fashion.
 * The code is provided only to help a developer better understand how
 * the SDK can be used.
 *
 * Copyright (c) 2008, Juniper Networks, Inc.
 * All rights reserved.
 */

/**
 * @file monitube-data_conn.c
 * @brief Relating to managing the connections
 *
 * These functions and types will manage the connections.
 */


#include "monitube-data_main.h"
#include <jnx/pconn.h>
#include "monitube-data_config.h"
#include "monitube-data_conn.h"


/*** Constants ***/

/**
 * Retry connecting to a component every RETRY_CONNECT if connection is lost
 */
#define RETRY_CONNECT 60
#define CONNECT_RETRIES    1  ///< max number of connect retries


/*** Data Structures ***/

extern volatile boolean is_master; ///< mastership state of this data component

/**
 * A notification message to store in the queue of message to go out to the
 * mgmt component
 */
typedef struct notification_msg_s {
    msg_type_e     action;   ///< action to send (MSG_FLOW_STAT_UPDATE for now)
    flow_stat_t *  message;  ///< data to send
    TAILQ_ENTRY(notification_msg_s) entries;    ///< ptrs to next/prev
} notification_msg_t;


/**
 * List of notification messages to buffer until main thread processes msgs
 */
static TAILQ_HEAD(notification_buffer_s, notification_msg_s) notification_msgs =
            TAILQ_HEAD_INITIALIZER(notification_msgs);

static msp_spinlock_t msgs_lock;     ///< lock for accessing notification_msgs

static pconn_client_t * mgmt_client; ///< client cnx to mgmt component

static evTimerID  mgmt_timer_id;     ///< timerID for retrying cnx to mgmt

static evContext  main_ctx;          ///< event context for main thread

/*** STATIC/INTERNAL Functions ***/


/**
 * Fwd declaration of function to setup the connection to the mgmt component
 */
static void connect_mgmt(evContext ctx, void * uap,
    struct timespec due, struct timespec inter);


/**
 * Go through the buffered messages and send them to the mgmt component
 * if it's connected.
 *
 * @param[in] ctx
 *     The event context for this application
 *
 * @param[in] uap
 *     The user data for this callback
 *
 * @param[in] due
 *     The absolute time when the event is due (now)
 *
 * @param[in] inter
 *     The period; when this will next be called
 */
static void
process_notifications_messages(evContext ctx UNUSED, void * uap  UNUSED,
            struct timespec due UNUSED, struct timespec inter UNUSED)
{
    notification_msg_t * msg;
    int rc = 0;

    if(mgmt_client == NULL) { // don't bother doing anything yet
        return;
    }

    INSIST_ERR(msp_spinlock_lock(&msgs_lock) == MSP_OK);

    while((msg = TAILQ_FIRST(&notification_msgs)) != NULL) {

        TAILQ_REMOVE(&notification_msgs, msg, entries);

        INSIST_ERR(msp_spinlock_unlock(&msgs_lock) == MSP_OK);

        rc = pconn_client_send(mgmt_client, msg->action, msg->message,
                sizeof(flow_stat_t) + msg->message->mon_name_len);

        if(rc != PCONN_OK) {
            // put message back in and process soon

            LOG(LOG_ERR, "%s: Failed to send message to mgmt component."
                    " Error: %d", __func__, rc);

            INSIST_ERR(msp_spinlock_lock(&msgs_lock) == MSP_OK);

            TAILQ_INSERT_HEAD(&notification_msgs, msg, entries);

            INSIST_ERR(msp_spinlock_unlock(&msgs_lock) == MSP_OK);

            if(evSetTimer(main_ctx, process_notifications_messages, NULL,
                    evAddTime(evNowTime(), evConsTime(5, 0)),
                    evConsTime(0, 0), NULL)) {

                LOG(LOG_EMERG, "%s: evSetTimer() failed! Will not be "
                        "able to process buffered notifications", __func__);
            }
        } else {
            free(msg->message);
            free(msg);
        }

        INSIST_ERR(msp_spinlock_lock(&msgs_lock) == MSP_OK);
    }

    INSIST_ERR(msp_spinlock_unlock(&msgs_lock) == MSP_OK);
}


/**
 * Try to process all notification requests that have been buffered.
 * This should be called with a notify* call below
 */
static void
process_notifications(void)
{
    if(evSetTimer(main_ctx, process_notifications_messages, NULL,
            evConsTime(0,0), evConsTime(0, 0), NULL)) {

        LOG(LOG_EMERG, "%s: evSetTimer() failed! Will not be "
                "able to process buffered notifications", __func__);
    }
}

/**
 * Connection handler for new and dying connections to the mgmt component
 *
 * @param[in] client
 *      The opaque client information for the source peer
 *
 * @param[in] event
 *      The event (established, or shutdown are the ones we care about)
 *
 * @param[in] cookie
 *      user data passed back
 */
static void
mgmt_client_connection(pconn_client_t * client,
                      pconn_event_t event,
                      void * cookie UNUSED)
{
    INSIST_ERR(client == mgmt_client);

    switch (event) {

    case PCONN_EVENT_ESTABLISHED:

        // clear the retry timer
        if(evTestID(mgmt_timer_id)) {
            evClearTimer(main_ctx, mgmt_timer_id);
            evInitID(&mgmt_timer_id);
        }

        clear_config(); // we expect to get a fresh copy of everything
        
        // For now, toggle this here until MASTERSHIP messages from the 
        // mgmt component are supported
        is_master = TRUE;
        
        LOG(LOG_INFO, "%s: Connected to the monitube-mgmt component "
                "on the RE", __func__);

        break;

    case PCONN_EVENT_SHUTDOWN:

        if(mgmt_client != NULL) {
            clear_config();
            LOG(LOG_INFO, "%s: Disconnected from the monitube-mgmt component"
                    " on the RE", __func__);
        }

        mgmt_client = NULL; // connection will be closed
        // For now, toggle this here until MASTERSHIP messages from the 
        // mgmt component are supported
        is_master = FALSE;

        // Reconnect to it if timer not going
        if(!evTestID(mgmt_timer_id) &&
           evSetTimer(main_ctx, connect_mgmt, NULL, evConsTime(0,0),
               evConsTime(RETRY_CONNECT, 0), &mgmt_timer_id)) {

            LOG(LOG_EMERG, "%s: Failed to initialize a re-connect timer to "
                "reconnect to the mgmt component", __func__);
        }

        break;

    case PCONN_EVENT_FAILED:

        LOG(LOG_ERR, "%s: Received a PCONN_EVENT_FAILED event", __func__);

        if(mgmt_client != NULL) {
            clear_config();
            LOG(LOG_INFO, "%s: Disconnected from the monitube-mgmt component"
                    " on the RE", __func__);
        }

        mgmt_client = NULL; // connection will be closed
        // For now, toggle this here until MASTERSHIP messages from the 
        // mgmt component are supported
        is_master = FALSE;

        // Reconnect to it if timer not going
        if(!evTestID(mgmt_timer_id) &&
           evSetTimer(main_ctx, connect_mgmt, NULL, evConsTime(0,0),
               evConsTime(RETRY_CONNECT, 0), &mgmt_timer_id)) {

            LOG(LOG_EMERG, "%s: Failed to initialize a re-connect timer to "
                "reconnect to the mgmt component", __func__);
        }

        break;

    default:
        LOG(LOG_ERR, "%s: Received an unknown pconn event", __func__);
    }
}


/**
 * Message handler for open connection to the mgmt component
 *
 * @param[in] session
 *      The session information for the source peer
 *
 * @param[in] msg
 *      The inbound message
 *
 * @param[in] cookie
 *      The cookie we passed in.
 *
 * @return
 *      SUCCESS if successful; otherwise EFAIL with an error message.
 */
static status_t
mgmt_client_message(pconn_client_t * session,
                    ipc_msg_t * msg,
                    void * cookie UNUSED)
{
    struct in_addr ia;

    INSIST_ERR(session == mgmt_client);
    INSIST_ERR(msg != NULL);

    switch(msg->subtype) { // a msg_type_e

    case MSG_CONF_MASTER:

        INSIST_ERR(msg->length == 0);

        LOG(LOG_INFO, "%s: Received a configuration update: Becoming master",
                __func__);

        set_mastership(TRUE, 0);

        break;

    case MSG_CONF_SLAVE:
    {
        slave_info_t * data = (slave_info_t *)msg->data;

        INSIST_ERR(msg->length == sizeof(slave_info_t));

        ia.s_addr = data->master_address;
        LOG(LOG_INFO, "%s: Received a configuration update: Becoming slave "
                "to %s", __func__, inet_ntoa(ia));

        set_mastership(FALSE, data->master_address);

        break;
    }
    case MSG_REP_INFO:
    {
        replication_info_t * data = (replication_info_t *)msg->data;

        INSIST_ERR(msg->length == sizeof(replication_info_t));

        LOG(LOG_INFO, "%s: Received a configuration update: Set replication "
                "interval: %d", __func__, data->interval);

        set_replication_interval(data->interval);

        break;
    }
    case MSG_DELETE_ALL_MON:

        INSIST_ERR(msg->length == 0);

        LOG(LOG_INFO, "%s: Received a configuration update: "
                "clear monitors configuration", __func__);

        clear_monitors_configuration();

        break;

    case MSG_DELETE_ALL_MIR:

        INSIST_ERR(msg->length == 0);

        LOG(LOG_INFO, "%s: Received a configuration update: "
                "clear mirrors configuration", __func__);

        clear_mirrors_configuration();

        break;

    case MSG_DELETE_MON:
    {
        del_mon_info_t * data = (del_mon_info_t *)msg->data;

        INSIST_ERR(msg->length == sizeof(del_mon_info_t) +
                ntohs(data->mon_name_len));

        LOG(LOG_INFO, "%s: Received a configuration update: delete monitor",
                __func__);

        delete_monitor(data->mon_name);

        break;
    }
    case MSG_DELETE_MIR:
    {
        del_mir_info_t * data = (del_mir_info_t *)msg->data;
        INSIST_ERR(msg->length == sizeof(del_mir_info_t));

        LOG(LOG_INFO, "%s: Received a configuration update: delete mirror",
                __func__);

        delete_mirror(data->mirror_from);

        break;
    }
    case MSG_DELETE_MON_ADDR:
    {
        maddr_info_t * data = (maddr_info_t *)msg->data;
        INSIST_ERR(msg->length ==
            sizeof(maddr_info_t) + ntohs(data->mon_name_len));

        LOG(LOG_INFO, "%s: Received a configuration update: "
                "delete monitored address (prefix)", __func__);

        delete_address(data->mon_name, data->addr, data->mask);

        break;
    }
    case MSG_CONF_MON:
    {
        update_mon_info_t * data = (update_mon_info_t *)msg->data;
        INSIST_ERR(msg->length ==
            sizeof(update_mon_info_t) + ntohs(data->mon_name_len));

        LOG(LOG_INFO, "%s: Received a configuration update: update "
            "monitor: %s/%d", __func__, data->mon_name, ntohl(data->rate));

        update_monitor(data->mon_name, ntohl(data->rate));

        break;
    }
    case MSG_CONF_MIR:
    {
        update_mir_info_t * data = (update_mir_info_t *)msg->data;
        INSIST_ERR(msg->length == sizeof(update_mir_info_t));

        LOG(LOG_INFO, "%s: Received a configuration update: update mirror",
            __func__);

        update_mirror(data->mirror_from, data->mirror_to);

        break;
    }
    case MSG_CONF_MON_ADDR:
    {
        maddr_info_t * data = (maddr_info_t *)msg->data;
        INSIST_ERR(msg->length ==
            sizeof(maddr_info_t) + ntohs(data->mon_name_len));

        LOG(LOG_INFO, "%s: Received a configuration update: "
                "add monitored address/prefix", __func__);
        ia.s_addr = data->addr;
        LOG(LOG_INFO, "%s: (%s with address: %s)", __func__,
                data->mon_name, inet_ntoa(ia));
        ia.s_addr = data->mask;
        LOG(LOG_INFO, "%s: (%s with mask: %s)", __func__,
                data->mon_name, inet_ntoa(ia));

        add_address(data->mon_name, data->addr, data->mask);

        break;
    }
    default:
        LOG(LOG_ERR, "%s: Received an unknown message type (%d) from the "
            "mgmt component", __func__, msg->subtype);
        return EFAIL;
    }

    return SUCCESS;
}


/**
 * Setup the connection to the mgmt component
 *
 * @param[in] ctx
 *     The event context for this application
 *
 * @param[in] uap
 *     The user data for this callback
 *
 * @param[in] due
 *     The absolute time when the event is due (now)
 *
 * @param[in] inter
 *     The period; when this will next be called
 */
static void
connect_mgmt(evContext ctx UNUSED,
            void * uap  UNUSED,
            struct timespec due UNUSED,
            struct timespec inter UNUSED)
{
    pconn_client_params_t params;

    bzero(&params, sizeof(pconn_client_params_t));

    // setup the client args
    params.pconn_peer_info.ppi_peer_type = PCONN_PEER_TYPE_RE;
    params.pconn_port                    = MONITUBE_PORT_NUM;
    params.pconn_num_retries             = CONNECT_RETRIES;
    params.pconn_event_handler           = mgmt_client_connection;

    if(mgmt_client) {
        // Then this is the second time in a row this is called even though it
        // didn't fail. We haven't received an event like ESTABLISHED yet.
        pconn_client_close(mgmt_client);
    }

    // connect
    mgmt_client = pconn_client_connect_async(
                    &params, main_ctx, mgmt_client_message, NULL);

    if(mgmt_client == NULL) {
        LOG(LOG_ERR, "%s: Failed to initialize the pconn client connection "
            "to the mgmt component", __func__);
    }

    LOG(LOG_INFO, "%s: Trying to connect to the mgmt component", __func__);
}


/*** GLOBAL/EXTERNAL Functions ***/


/**
 * Initialize the connection to the mgmt component
 *
 * @param[in] ctx
 *      event context
 *
 * @return
 *      SUCCESS if successful; otherwise EFAIL with an error message.
 */
status_t
init_connections(evContext ctx)
{
    mgmt_client = NULL;

    main_ctx = ctx;

    evInitID(&mgmt_timer_id);
    msp_spinlock_init(&msgs_lock);

    // Connect to the MGMT component
    if(evSetTimer(ctx, connect_mgmt, NULL, evNowTime(),
        evConsTime(RETRY_CONNECT, 0), &mgmt_timer_id)) {

        LOG(LOG_EMERG, "%s: Failed to initialize a connect timer to connect "
            "to the MGMT component", __func__);
        return EFAIL;
    }

    return SUCCESS;
}


/**
 * Terminate connection to the mgmt component
 */
void
close_connections(void)
{
    notification_msg_t * msg;

    if(mgmt_client) {
        pconn_client_close(mgmt_client);
        mgmt_client = NULL;
    }

    INSIST_ERR(msp_spinlock_lock(&msgs_lock) == MSP_OK);

    while((msg = TAILQ_FIRST(&notification_msgs)) != NULL) {
        TAILQ_REMOVE(&notification_msgs, msg, entries);
        free(msg->message);
        free(msg);
    }

    INSIST_ERR(msp_spinlock_unlock(&msgs_lock) == MSP_OK);
}


/**
 * Notify the mgmt component about a statistic update
 *
 * @param[in] flow_addr
 *      flow address (id)
 *
 * @param[in] flow_dport
 *      flow dst port (in net. byte-order) (id)
 *
 * @param[in] mdi_df
 *      the delay factor
 *
 * @param[in] mdi_mlr
 *      the media loss rate
 *
 * @param[in] monitor_name
 *      the monitor name
 */
void
notify_stat_update(in_addr_t flow_addr,
                   uint16_t flow_dport,
                   double mdi_df,
                   uint32_t mdi_mlr,
                   char * monitor_name)
{
    notification_msg_t * msg;
    uint16_t len;
    void * doub;
    uint64_t out;

    if(mgmt_client == NULL) { // don't bother doing anything yet
        return;
    }

    msg = calloc(1, sizeof(notification_msg_t));
    INSIST_ERR(msg != NULL);

    msg->action = MSG_FLOW_STAT_UPDATE;

    len = strlen(monitor_name);
    msg->message = calloc(1, sizeof(flow_stat_t) + len + 1);
    msg->message->flow_addr = flow_addr;
    msg->message->flow_port = flow_dport;
    doub = &mdi_df; // play around double type in case of a truncating typecast
    out = *(uint64_t *)doub;
    msg->message->mdi_df = htonq(out);
    msg->message->mdi_mlr = mdi_mlr;
    msg->message->mon_name_len = len + 1;
    strcpy(msg->message->mon_name, monitor_name);

    INSIST_ERR(msp_spinlock_lock(&msgs_lock) == MSP_OK);

    TAILQ_INSERT_TAIL(&notification_msgs, msg, entries);

    INSIST_ERR(msp_spinlock_unlock(&msgs_lock) == MSP_OK);

    process_notifications();
}
