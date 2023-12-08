// Copyright (c) 2021 MIT Digital Currency Initiative,
//                    Federal Reserve Bank of Boston
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "controller.hpp"

#include "uhs/twophase/coordinator/format.hpp"
#include "util/rpc/tcp_server.hpp"
#include "util/serialization/util.hpp"

#include <utility>
#include "oracleDB.h"


OracleDB db;

namespace cbdc::sentinel_2pc {
    controller::controller(uint32_t sentinel_id,
                           const config::options& opts,
                           std::shared_ptr<logging::log> logger)
        : m_sentinel_id(sentinel_id),
          m_opts(opts),
          m_logger(std::move(logger)),
          m_coordinator_client(
              opts.m_coordinator_endpoints[sentinel_id
                                           % static_cast<uint32_t>(
                                               opts.m_coordinator_endpoints
                                                   .size())]) 
        {
        }

    auto controller::init() -> bool {
        
            // Connecting to Oracle Autonomous Database
            if (OracleDB_init(&db) == 0) {
                if (OracleDB_connect(&db) == 0) {
                    m_logger->info("Connected to Oracle Autonomous Database");
                } else {
                    m_logger->error("Failed to connect to Oracle Autonomous Database");
                }
            }
               
        if(m_opts.m_sentinel_endpoints.empty()) {
            m_logger->error("No sentinel endpoints are defined.");
            return false;
        }

        if(m_sentinel_id >= m_opts.m_sentinel_endpoints.size()) {
            m_logger->error(
                "The sentinel ID is too large for the number of sentinels.");
            return false;
        }

        auto skey = m_opts.m_sentinel_private_keys.find(m_sentinel_id);
        if(skey == m_opts.m_sentinel_private_keys.end()) {
            if(m_opts.m_attestation_threshold > 0) {
                m_logger->error("No private key specified");
                return false;
            }
        } else {
            m_privkey = skey->second;

            auto pubkey = pubkey_from_privkey(m_privkey, m_secp.get());
            m_logger->info("Sentinel public key:", cbdc::to_string(pubkey));
        }

        auto retry_delay = std::chrono::seconds(1);
        auto retry_threshold = 4;
        while(!m_coordinator_client.init() && retry_threshold-- > 0) {
            m_logger->warn("Failed to start coordinator client.");

            std::this_thread::sleep_for(retry_delay);
            if(retry_threshold > 0) {
                retry_delay *= 2;
                m_logger->warn("Retrying...");
            }
        }

        for(const auto& ep : m_opts.m_sentinel_endpoints) {
            if(ep == m_opts.m_sentinel_endpoints[m_sentinel_id]) {
                continue;
            }
            auto client = std::make_unique<sentinel::rpc::client>(
                std::vector<network::endpoint_t>{ep},
                m_logger);
            if(!client->init(false)) {
                m_logger->warn("Failed to start sentinel client");
            }
            m_sentinel_clients.emplace_back(std::move(client));
        }

        constexpr size_t dist_lower_bound = 0;
        const size_t dist_upper_bound
            = m_sentinel_clients.empty() ? 0 : m_sentinel_clients.size() - 1;
        m_dist = decltype(m_dist)(dist_lower_bound, dist_upper_bound);

        auto rpc_server = std::make_unique<cbdc::rpc::tcp_server<
            cbdc::rpc::async_server<cbdc::sentinel::request,
                                    cbdc::sentinel::response>>>(
            m_opts.m_sentinel_endpoints[m_sentinel_id]);
        if(!rpc_server->init()) {
            m_logger->error("Failed to start sentinel RPC server");
            return false;
        }

        m_rpc_server = std::make_unique<decltype(m_rpc_server)::element_type>(
            this,
            std::move(rpc_server));

        return true;
    }

    auto controller::execute_transaction(
        transaction::full_tx tx,
        execute_result_callback_type result_callback) -> bool {
        const auto validation_err = transaction::validation::check_tx(tx);
        if(validation_err.has_value()) {
            auto tx_id = transaction::tx_id(tx);
            m_logger->debug(
                "Rejected (",
                transaction::validation::to_string(validation_err.value()),
                ")",
                to_string(tx_id));
            result_callback(cbdc::sentinel::execute_response{
                cbdc::sentinel::tx_status::static_invalid,
                validation_err});
            return true;
        }

        auto compact_tx = cbdc::transaction::compact_tx(tx);

        if(m_opts.m_attestation_threshold > 0) {
            auto attestation = compact_tx.sign(m_secp.get(), m_privkey);
            compact_tx.m_attestations.insert(attestation);
        }

        gather_attestations(tx, std::move(result_callback), compact_tx, {});

        return true;
    }

    void
    controller::result_handler(std::optional<bool> res,
                               const execute_result_callback_type& res_cb) {
        if(res.has_value()) {
            auto resp = cbdc::sentinel::execute_response{
                cbdc::sentinel::tx_status::confirmed,
                std::nullopt};
            if(!res.value()) {
                resp.m_tx_status = cbdc::sentinel::tx_status::state_invalid;
            }
            res_cb(resp);
        } else {
            res_cb(std::nullopt);
        }
    }

    auto controller::validate_transaction(
        transaction::full_tx tx,
        validate_result_callback_type result_callback) -> bool {
        const auto validation_err = transaction::validation::check_tx(tx);
        if(validation_err.has_value()) {
            result_callback(std::nullopt);
            return true;
        }
        auto compact_tx = cbdc::transaction::compact_tx(tx);
        auto attestation = compact_tx.sign(m_secp.get(), m_privkey);
        result_callback(std::move(attestation));
        return true;
    }

    void controller::validate_result_handler(
        validate_result v_res,
        const transaction::full_tx& tx,
        execute_result_callback_type result_callback,
        transaction::compact_tx ctx,
        std::unordered_set<size_t> requested) {
        if(!v_res.has_value()) {
            m_logger->error(to_string(ctx.m_id),
                            "invalid according to remote sentinel");
            result_callback(std::nullopt);
            return;
        }
        ctx.m_attestations.insert(std::move(v_res.value()));
        gather_attestations(tx,
                            std::move(result_callback),
                            ctx,
                            std::move(requested));
    }

    void controller::gather_attestations(
        const transaction::full_tx& tx,
        execute_result_callback_type result_callback,
        const transaction::compact_tx& ctx,
        std::unordered_set<size_t> requested) {
        if(ctx.m_attestations.size() < m_opts.m_attestation_threshold) {
            auto success = false;
            while(!success) {
                auto sentinel_id = m_dist(m_rand);
                if(requested.find(sentinel_id) != requested.end()) {
                    continue;
                }
                success
                    = m_sentinel_clients[sentinel_id]->validate_transaction(
                        tx,
                        [=](validate_result v_res) {
                            auto r = requested;
                            r.insert(sentinel_id);
                            validate_result_handler(v_res,
                                                    tx,
                                                    result_callback,
                                                    ctx,
                                                    r);
                        });
            }
            return;
        }

        m_logger->debug("Accepted", to_string(ctx.m_id));

        send_compact_tx(ctx, std::move(result_callback));
    }

    void
    controller::send_compact_tx(const transaction::compact_tx& ctx,
                                execute_result_callback_type result_callback) {
        auto cb =
            [&, res_cb = std::move(result_callback)](std::optional<bool> res) {
                result_handler(res, res_cb);
            };

        // TODO: add a "retry" error response to offload sentinels from this
        //       infinite retry responsibility.
        while(!m_coordinator_client.execute_transaction(ctx, cb)) {
            // TODO: the network currently doesn't provide a callback for
            //       reconnection events so we have to sleep here to
            //       prevent a needless spin. Instead, add such a callback
            //       or queue to the network to remove this sleep.
            static constexpr auto retry_delay = std::chrono::milliseconds(100);
            std::this_thread::sleep_for(retry_delay);
        };

        // adding DTX to Oracle Autonomous Database
        std::string dtx_string = std::string(ctx.m_id.begin(), ctx.m_id.end());
        m_logger->info("DTX: " + std::string(ctx.m_id.begin(), ctx.m_id.end()));
        std::string dtx_hex;
        dtx_hex.reserve(2*dtx_string.size());

        // convert dtx_string into a hex string
        for (unsigned char c : dtx_string) {
            dtx_hex.push_back("0123456789ABCDEF"[c >> 4]);
            dtx_hex.push_back("0123456789ABCDEF"[c & 15]);
        }
        m_logger->info("DTX HEX: " + dtx_hex);

        std::string dtx_hex_insert = "INSERT INTO admin.transaction (transactionhash, payee, amt) SELECT tx_hash, payee_to, amount FROM admin.transactionholder WHERE tx_hash = '" + dtx_hex + "'";

        if(OracleDB_execute(&db, dtx_hex_insert.c_str()) == 0) {
            m_logger->info("Inserted DTX Hex into admin.transaction from controller.cpp");
        } else {
            m_logger->error("Failed to insert DTX Hex into admin.transaction from controller.cpp");
        }


        for(hash_t input_hash: ctx.m_inputs){
            std::string in_string = std::string(input_hash.begin(), input_hash.end());
            std::string in_hex;
            in_hex.reserve(2*input_hash.size());

            for (unsigned char c : in_string) {
                in_hex.push_back("0123456789ABCDEF"[c >> 4]);
                in_hex.push_back("0123456789ABCDEF"[c & 15]);
            }

            std::string dtx_inputs_insert = "INSERT INTO admin.input (transactionhash, uhshash) VALUES ('" + dtx_hex + "', '" + in_hex + "')";
            if(OracleDB_execute(&db, dtx_inputs_insert.c_str()) == 0) {
                m_logger->info("Inserted DTX Inputs into admin.input from controller.cpp");
            } else {
                m_logger->error("Failed to insert DTX Inputs into admin.input from controller.cpp");
            }

            std::string dtx_inputs_insert = "DELETE FROM admin.uhs_previews where UHS_HASH =  '"+ in_hex + "'";
            if(OracleDB_execute(&db, dtx_inputs_insert.c_str()) == 0) {
                m_logger->info("Removed DTX Inputs into admin.uhs_previews from controller.cpp");
            } else {
                m_logger->error("Could not remove DTX Inputs into admin.uhs_previews from controller.cpp");
            }


        }

        for(hash_t output_hash: ctx.m_uhs_outputs){
            std::string out_string = std::string(output_hash.begin(), output_hash.end());
            std::string out_hex;
            out_hex.reserve(2*output_hash.size());

            // convert dtx_string into a hex string
            for (unsigned char c : out_string) {
                out_hex.push_back("0123456789ABCDEF"[c >> 4]);
                out_hex.push_back("0123456789ABCDEF"[c & 15]);
            }

            std::string dtx_outputs_insert = "INSERT INTO admin.output (transactionhash, uhshash) VALUES ('" + dtx_hex + "', '" + out_hex + "')";
            if(OracleDB_execute(&db, dtx_outputs_insert.c_str()) == 0) {
                m_logger->info("Inserted DTX Outputs into admin.output from controller.cpp");
            } else {
                m_logger->error("Failed to insert DTX Outputs into admin.output from controller.cpp");
            }


            std::string dtx_outputs_insert = "INSERT INTO admin.uhs_previews (uhshash) VALUES ('" + out_hex + "')";
            if(OracleDB_execute(&db, dtx_outputs_insert.c_str()) == 0) {
                m_logger->info("Inserted DTX Outputs into admin.uhs_previews from controller.cpp");
            } else {
                m_logger->error("Failed to insert DTX Outputs into admin.uhs_previews from controller.cpp");
            }

        }
    }
}
