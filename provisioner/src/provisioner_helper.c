/* Copyright (c) 2010 - 2018, Nordic Semiconductor ASA
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form, except as embedded into a Nordic
 *    Semiconductor ASA integrated circuit in a product or a software update for
 *    such product, must reproduce the above copyright notice, this list of
 *    conditions and the following disclaimer in the documentation and/or other
 *    materials provided with the distribution.
 *
 * 3. Neither the name of Nordic Semiconductor ASA nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * 4. This software, with or without modification, must only be used with a
 *    Nordic Semiconductor ASA integrated circuit.
 *
 * 5. Any software provided in binary form under this license must not be reverse
 *    engineered, decompiled, modified and/or disassembled.
 *
 * THIS SOFTWARE IS PROVIDED BY NORDIC SEMICONDUCTOR ASA "AS IS" AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL NORDIC SEMICONDUCTOR ASA OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "example_common.h"
#include "nrf_mesh_prov.h"
#include "nrf_mesh_prov_bearer_adv.h"
#include "nrf_mesh_events.h"
#include "mesh_app_utils.h"
#include "nrf_mesh_config_app.h"
#include "provisioner_helper.h"
#include "node_setup.h"
#include "rand.h"
#include "log.h"
#include "enc.h"

typedef enum
{
    PROV_STATE_IDLE,
    PROV_STATE_WAIT,
    PROV_STATE_COMPLETE,
    PROV_STATE_PROV
} prov_state_t;

static uint8_t m_public_key[NRF_MESH_PROV_PUBKEY_SIZE];
static uint8_t m_private_key[NRF_MESH_PROV_PRIVKEY_SIZE];

static nrf_mesh_prov_bearer_adv_t m_prov_bearer_adv;
static nrf_mesh_prov_ctx_t m_prov_ctx;
static const char **mp_uri_filter;
static const char *mp_current_uri;
static uint8_t m_uri_count;

static prov_state_t m_prov_state;
static uint8_t      m_retry_cnt;

static uint16_t m_target_address;
static uint16_t m_target_elements;

static mesh_provisioner_init_params_t m_provisioner;
static bool m_provisioner_init_done;

/* Forward declaration */
static void prov_evt_handler(const nrf_mesh_prov_evt_t * p_evt);

/* Compare the given URI hash with the calculated URI hash. If invalid inputs are
provided this function will always return false. */
static bool uri_hash_compare(const uint8_t * p_in_uri_hash, const char * p_uri)
{
    uint8_t m_uri_hash[NRF_MESH_UUID_SIZE];

    if (p_in_uri_hash == NULL || p_uri == NULL)
    {
        return false;
    }

    enc_s1((const uint8_t*) p_uri, strlen(p_uri), m_uri_hash);

    return (memcmp(p_in_uri_hash, m_uri_hash, NRF_MESH_BEACON_UNPROV_URI_HASH_SIZE) == 0);
}

static void start_provisioning(const uint8_t * p_uuid)
{
    nrf_mesh_prov_provisioning_data_t prov_data =
        {
            .netkey_index = m_provisioner.netkey_idx,
            .iv_index = 0,
            .address = m_target_address,
            .flags.iv_update = false,
            .flags.key_refresh = false
        };
    memcpy(prov_data.netkey, m_provisioner.p_nw_data->netkey, NRF_MESH_KEY_SIZE);
    ERROR_CHECK(nrf_mesh_prov_provision(&m_prov_ctx, p_uuid, m_provisioner.attention_duration_s, &prov_data, NRF_MESH_PROV_BEARER_ADV));
}

static void prov_helper_provisioner_init(void)
{
    if (!m_provisioner_init_done)
    {
        nrf_mesh_prov_oob_caps_t capabilities = NRF_MESH_PROV_OOB_CAPS_DEFAULT(ACCESS_ELEMENT_COUNT);

        ERROR_CHECK(nrf_mesh_prov_generate_keys(m_public_key, m_private_key));
        ERROR_CHECK(nrf_mesh_prov_init(&m_prov_ctx, m_public_key, m_private_key, &capabilities, prov_evt_handler));
        ERROR_CHECK(nrf_mesh_prov_bearer_add(&m_prov_ctx, nrf_mesh_prov_bearer_adv_interface_get(&m_prov_bearer_adv)));
    }

    m_provisioner_init_done = true;
}

/* Provisioning process event handling */
static void prov_evt_handler(const nrf_mesh_prov_evt_t * p_evt)
{
    static dsm_handle_t addr_handle;
    static dsm_handle_t devkey_handle;

    switch (p_evt->type)
    {
        case NRF_MESH_PROV_EVT_UNPROVISIONED_RECEIVED:
            if (m_prov_state == PROV_STATE_WAIT)
            {
                __LOG_XB(LOG_SRC_APP, LOG_LEVEL_INFO, "UUID ", p_evt->params.unprov.device_uuid, NRF_MESH_UUID_SIZE);
                if (p_evt->params.unprov.p_metadata->source == NRF_MESH_RX_SOURCE_SCANNER)
                {
                    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "^RSSI: %d\n",
                          p_evt->params.unprov.p_metadata->params.scanner.rssi);
                }

                if (p_evt->params.unprov.uri_hash_present)
                {
                    __LOG_XB(LOG_SRC_APP, LOG_LEVEL_INFO, "^URI Hash", p_evt->params.unprov.uri_hash, NRF_MESH_BEACON_UNPROV_URI_HASH_SIZE);

                    for (uint32_t i = 0; i < m_uri_count; i++)
                    {
                        if (uri_hash_compare(p_evt->params.unprov.uri_hash, mp_uri_filter[i]))
                        {
                            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "URI hash matched. Provisioning ...\n");

                            mp_current_uri = mp_uri_filter[i];
                            start_provisioning(p_evt->params.unprov.device_uuid);
                            m_prov_state = PROV_STATE_PROV;
                            break;
                        }
                    }
                }
            }
            break;

        case NRF_MESH_PROV_EVT_LINK_CLOSED:
            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Local provisioning link closed: prov_state: %d  remaining retries: %d\n", m_prov_state, m_retry_cnt);
            if (m_prov_state == PROV_STATE_PROV)
            {
                if (m_retry_cnt)
                {
                    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Provisioning failed. Retrying...\n");
                    m_prov_state = PROV_STATE_WAIT;
                    m_retry_cnt--;
                }
                else
                {
                    __LOG(LOG_SRC_APP, LOG_LEVEL_ERROR, "Provisioning Failed. Code: %d, Could not assign node addr: 0x%04x\n",
                          p_evt->params.failed.failure_code, m_target_address);
                    m_prov_state = PROV_STATE_IDLE;
                    m_provisioner.p_prov_failed_cb();
                }
            }
            else if (m_prov_state == PROV_STATE_COMPLETE)
            {
                m_provisioner.p_nw_data->last_device_address = m_target_address;
                m_provisioner.p_nw_data->provisioned_devices++;
                m_provisioner.p_nw_data->next_device_address += m_target_elements;
                if (m_provisioner.p_nw_data->last_device_address == UNPROV_START_ADDRESS)
                {
                    m_provisioner.p_nw_data->p_client_uri = mp_current_uri;
                }
                m_provisioner.p_data_store_cb();
                m_provisioner.p_prov_success_cb();

                __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Provisioning complete. Node addr: 0x%04x elements: %d\n",
                      m_provisioner.p_nw_data->last_device_address, m_target_elements);

                node_setup_start(m_provisioner.p_nw_data->last_device_address, PROVISIONER_RETRY_COUNT,
                m_provisioner.p_nw_data->appkey, APPKEY_INDEX, m_provisioner.p_nw_data->p_client_uri);
                m_prov_state = PROV_STATE_IDLE;

            }
            break;

        case NRF_MESH_PROV_EVT_COMPLETE:
        {
            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Provisioning completed received\n");
            m_prov_state = PROV_STATE_COMPLETE;

            /* After provisioning completes, add corresponding node's address and device key to
            local database */
            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Adding device address, and device keys\n");
            ERROR_CHECK(dsm_address_publish_add(p_evt->params.complete.p_prov_data->address,
                                                &addr_handle));
            ERROR_CHECK(dsm_devkey_add(p_evt->params.complete.p_prov_data->address,
                                       m_provisioner.p_dev_data->m_netkey_handle,
                                       p_evt->params.complete.p_devkey,
                                       &devkey_handle));

            /* Bind the device key to the configuration server and set the new node as the active server. */
            ERROR_CHECK(config_client_server_bind(devkey_handle));
            ERROR_CHECK(config_client_server_set(devkey_handle,
                                                addr_handle));

            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Addr: 0x%04x addr_handle: %d netkey_handle: %d devkey_handle: %d\n",
                  p_evt->params.complete.p_prov_data->address,
                  addr_handle,
                  m_provisioner.p_dev_data->m_netkey_handle,
                  devkey_handle);

            break;
        }

        case NRF_MESH_PROV_EVT_CAPS_RECEIVED:
        {
            m_target_elements = p_evt->params.oob_caps_received.oob_caps.num_elements;

            /* This example uses static OOB only. */
            uint32_t status = nrf_mesh_prov_oob_use(p_evt->params.oob_caps_received.p_context,
                                                    NRF_MESH_PROV_OOB_METHOD_STATIC,
                                                    0,
                                                    NRF_MESH_KEY_SIZE);
            if (status != NRF_SUCCESS)
            {
                __LOG(LOG_SRC_APP, LOG_LEVEL_ERROR, "Provisioning OOB selection rejected. Error: %d\n", status);
                if (m_retry_cnt)
                {
                    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Retrying ...\n");
                    m_prov_state = PROV_STATE_WAIT;
                    m_retry_cnt--;
                }
                else
                {
                    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Provisioning Failed. Cannot select static OOB. \
                    Could not assign node addr: 0x%04x\n", m_target_address);

                    m_prov_state = PROV_STATE_IDLE;
                    m_provisioner.p_prov_failed_cb();
                }
            }

            break;
        }

        case NRF_MESH_PROV_EVT_STATIC_REQUEST:
        {
            const uint8_t static_data[NRF_MESH_KEY_SIZE] = STATIC_AUTH_DATA;
            ERROR_CHECK(nrf_mesh_prov_auth_data_provide(p_evt->params.static_request.p_context, static_data, NRF_MESH_KEY_SIZE));
            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Static authentication data provided\n");
            break;
        }

        case NRF_MESH_PROV_EVT_LINK_ESTABLISHED:
            __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Provisioning link established\n");
            break;

        default:
            break;
    }

}

void prov_helper_provision_next_device(uint8_t retry_cnt, uint16_t address, const char **p_uri_filter,
                                       uint8_t uri_count)
{
    if (p_uri_filter == NULL || uri_count == 0)
    {
        __LOG(LOG_SRC_APP, LOG_LEVEL_ERROR, "Cannot provision. No example URI identifier(s) provided.\n")
        return;
    }

    m_retry_cnt = retry_cnt;
    m_target_address = address;
    m_prov_state = PROV_STATE_WAIT;
    mp_uri_filter = p_uri_filter;
    m_uri_count = uri_count;

    prov_helper_provisioner_init();
}

void prov_helper_scan_start(void)
{
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "Scanning For Unprovisioned Devices\n");
    ERROR_CHECK(nrf_mesh_prov_scan_start(prov_evt_handler));
}

void prov_helper_device_handles_load(void)
{
    dsm_local_unicast_address_t local_addr;

    /* Load net key handles: This application has only 1 netkey */
    uint32_t count = 1;
    ERROR_CHECK(dsm_subnet_get_all(&m_provisioner.p_dev_data->m_netkey_handle, &count));
    NRF_MESH_ASSERT(count == 1);
    ERROR_CHECK(dsm_appkey_get_all(m_provisioner.p_dev_data->m_netkey_handle, &m_provisioner.p_dev_data->m_appkey_handle, &count));

    /* Load self address handle */
    dsm_local_unicast_addresses_get(&local_addr);
    ERROR_CHECK(dsm_devkey_handle_get(local_addr.address_start, &m_provisioner.p_dev_data->m_self_devkey_handle));

    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "m_netkey_handle:%d m_appkey_handle:%d m_self_devkey_handle:%d\n",
    m_provisioner.p_dev_data->m_netkey_handle, m_provisioner.p_dev_data->m_appkey_handle, m_provisioner.p_dev_data->m_self_devkey_handle);
}

void prov_helper_init(mesh_provisioner_init_params_t * p_prov_init_info)
{
    NRF_MESH_ASSERT(p_prov_init_info->p_data_store_cb != NULL);
    NRF_MESH_ASSERT(p_prov_init_info->p_prov_failed_cb != NULL);
    NRF_MESH_ASSERT(p_prov_init_info->p_prov_success_cb != NULL);

    m_provisioner = *p_prov_init_info;
    m_provisioner_init_done = false;
    m_prov_state = PROV_STATE_IDLE;
}

void prov_helper_provision_self(void)
{
    /* Add addresses */
    /* Set and add local addresses and keys, if flash recovery fails. */
    dsm_local_unicast_address_t local_address = {PROVISIONER_ADDRESS, ACCESS_ELEMENT_COUNT};
    ERROR_CHECK(dsm_local_unicast_addresses_set(&local_address));

    /* Generate keys */
    rand_hw_rng_get(m_provisioner.p_nw_data->netkey, NRF_MESH_KEY_SIZE);
    rand_hw_rng_get(m_provisioner.p_nw_data->appkey, NRF_MESH_KEY_SIZE);
    rand_hw_rng_get(m_provisioner.p_nw_data->self_devkey, NRF_MESH_KEY_SIZE);

    /* Add default Netkey and App Key */
    ERROR_CHECK(dsm_subnet_add(0, m_provisioner.p_nw_data->netkey, &m_provisioner.p_dev_data->m_netkey_handle));
    __LOG(LOG_SRC_APP, LOG_LEVEL_INFO, "netkey_handle: %d\n", m_provisioner.p_dev_data->m_netkey_handle);
    ERROR_CHECK(dsm_appkey_add(0, m_provisioner.p_dev_data->m_netkey_handle, m_provisioner.p_nw_data->appkey, &m_provisioner.p_dev_data->m_appkey_handle));

    /* Add device key for the own config server */
    ERROR_CHECK(dsm_devkey_add(PROVISIONER_ADDRESS, m_provisioner.p_dev_data->m_netkey_handle, m_provisioner.p_nw_data->self_devkey, &m_provisioner.p_dev_data->m_self_devkey_handle));

}

uint32_t prov_helper_element_count_get(void)
{
    return m_prov_ctx.capabilities.num_elements;
}

