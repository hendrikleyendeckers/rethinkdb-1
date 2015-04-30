// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "clustering/administration/tables/calculate_status.hpp"

#include <algorithm>

#include "clustering/administration/datum_adapter.hpp"
#include "clustering/administration/servers/config_client.hpp"
#include "clustering/table_contract/exec_primary.hpp"
#include "clustering/table_manager/table_meta_client.hpp"

bool get_contracts_and_acks(
        namespace_id_t const &table_id,
        signal_t *interruptor,
        table_meta_client_t *table_meta_client,
        server_config_client_t *server_config_client,
        std::map<server_id_t, contracts_and_contract_acks_t> *contracts_and_acks_out,
        std::map<
                contract_id_t,
                std::reference_wrapper<const std::pair<region_t, contract_t> >
            > *contracts_out,
        server_id_t *latest_contracts_server_id_out) {
    std::map<peer_id_t, contracts_and_contract_acks_t> contracts_and_acks;
    if (!table_meta_client->get_status(
            table_id, interruptor, nullptr, &contracts_and_acks)) {
        return false;
    }

    multi_table_manager_bcard_t::timestamp_t latest_timestamp;
    latest_timestamp.epoch.timestamp = 0;
    latest_timestamp.epoch.id = nil_uuid();
    latest_timestamp.log_index = 0;
    for (const auto &peer : contracts_and_acks) {
        boost::optional<server_id_t> server_id =
            server_config_client->get_server_id_for_peer_id(peer.first);
        if (static_cast<bool>(server_id)) {
            auto pair = contracts_and_acks_out->insert(
                std::make_pair(server_id.get(), std::move(peer.second)));
            contracts_out->insert(
                pair.first->second.contracts.begin(),
                pair.first->second.contracts.end());
            if (pair.first->second.timestamp.supersedes(latest_timestamp)) {
                *latest_contracts_server_id_out = pair.first->first;
                latest_timestamp = pair.first->second.timestamp;
            }
        }
    }

    return !contracts_and_acks_out->empty();
}

shard_status_t calculate_shard_status(
        const table_config_t::shard_t &shard,
        const region_map_t<region_acks_t> &regions,
        const std::map<server_id_t, contracts_and_contract_acks_t> &contracts_and_acks,
        const std::map<
                contract_id_t,
                std::reference_wrapper<const std::pair<region_t, contract_t> >
            > &contracts) {
    shard_status_t shard_status;

    bool has_quorum = true;
    bool has_primary_replica = true;
    bool has_outdated_reader = true;
    bool has_unfinished = false;

    for (const auto &region : regions) {
        const contract_t &latest_contract =
            contracts.at(region.second.latest_contract_id).get().second;

        ack_counter_t ack_counter(latest_contract);
        bool region_has_primary_replica = false;
        bool region_has_outdated_reader = false;

        for (const auto &ack : region.second.acks) {
            switch (ack.second.second.state) {
                case contract_ack_t::state_t::primary_need_branch:
                    has_unfinished = true;
                    shard_status.replicas[ack.first].insert(
                        server_status_t::WAITING_FOR_QUORUM);
                    break;
                case contract_ack_t::state_t::secondary_need_primary:
                    region_has_outdated_reader = true;
                    has_unfinished = true;
                    shard_status.replicas[ack.first].insert(
                        server_status_t::WAITING_FOR_PRIMARY);
                    break;
                case contract_ack_t::state_t::primary_in_progress:
                case contract_ack_t::state_t::primary_ready:
                    ack_counter.note_ack(ack.first);
                    region_has_primary_replica = true;
                    shard_status.primary_replicas.insert(ack.first);
                    shard_status.replicas[ack.first].insert(
                        server_status_t::READY);
                    break;
                case contract_ack_t::state_t::secondary_backfilling:
                    has_unfinished = true;
                    shard_status.replicas[ack.first].insert(
                        server_status_t::BACKFILLING);
                    break;
                case contract_ack_t::state_t::secondary_streaming:
                    {
                        const boost::optional<contract_t::primary_t> &region_primary =
                            contracts.at(ack.second.first).get().second.primary;
                        if (static_cast<bool>(latest_contract.primary) &&
                                latest_contract.primary == region_primary) {
                            ack_counter.note_ack(ack.first);
                            region_has_outdated_reader = true;
                            shard_status.replicas[ack.first].insert(
                                server_status_t::READY);
                        } else {
                            has_unfinished = true;
                            shard_status.replicas[ack.first].insert(
                                server_status_t::TRANSITIONING);
                        }
                    }
                    break;
                case contract_ack_t::state_t::nothing:
                    /* We don't want to show shards that are in the `nothing` state thus
                       we don't insert a string into `replicas`. However, to prevent
                       them from being marked as "transitioning" below we insert an
                       empty std::set if it doesn't exist yet. */
                    if (shard_status.replicas.find(ack.first) ==
                            shard_status.replicas.end()) {
                        shard_status.replicas[ack.first] = {};
                    }
                    break;
            }
        }

        for (const auto &replica : latest_contract.replicas) {
            if (shard_status.replicas.find(replica) == shard_status.replicas.end()) {
                has_unfinished = true;
                shard_status.replicas[replica].insert(
                    contracts_and_acks.find(replica) == contracts_and_acks.end()
                        ? server_status_t::DISCONNECTED
                        : server_status_t::TRANSITIONING);
            }
        }
        for (const auto &replica : shard.replicas) {
            if (shard_status.replicas.find(replica) == shard_status.replicas.end()) {
                has_unfinished = true;
                shard_status.replicas[replica].insert(
                    contracts_and_acks.find(replica) == contracts_and_acks.end()
                        ? server_status_t::DISCONNECTED
                        : server_status_t::TRANSITIONING);
            }
        }

        has_quorum &= ack_counter.is_safe();
        has_primary_replica &= region_has_primary_replica;
        has_outdated_reader &= region_has_outdated_reader;
    }

    if (has_primary_replica) {
        if (has_quorum) {
            if (!has_unfinished) {
                shard_status.readiness = table_readiness_t::finished;
            } else {
                shard_status.readiness = table_readiness_t::writes;
            }
        } else {
            shard_status.readiness = table_readiness_t::reads;
        }
    } else {
        if (has_outdated_reader) {
            shard_status.readiness = table_readiness_t::outdated_reads;
        } else {
            shard_status.readiness = table_readiness_t::unavailable;
        }
    }

    return shard_status;
}

bool calculate_status(
        const namespace_id_t &table_id,
        const table_config_and_shards_t &config_and_shards,
        signal_t *interruptor,
        table_meta_client_t *table_meta_client,
        server_config_client_t *server_config_client,
        table_readiness_t *readiness_out,
        region_map_t<shard_status_t> *shard_statuses_out,
        std::string *error_out) {
    /* Note that `contracts` and `latest_contracts` will contain references into
       `contracts_and_acks`, thus this must remain in scope for them to be valid! */
    std::map<server_id_t, contracts_and_contract_acks_t> contracts_and_acks;
    std::map<
            contract_id_t,
            std::reference_wrapper<const std::pair<region_t, contract_t> >
        > contracts;
    server_id_t latest_contracts_server_id;
    if (!get_contracts_and_acks(
            table_id,
            interruptor,
            table_meta_client,
            server_config_client,
            &contracts_and_acks,
            &contracts,
            &latest_contracts_server_id)) {
        if (error_out != nullptr) {
            *error_out = strprintf(
                "Lost contact with the server(s) hosting table `%s.%s`.",
                uuid_to_str(config_and_shards.config.database).c_str(),
                uuid_to_str(table_id).c_str());
        }
        return false;
    }
    const std::map<contract_id_t, std::pair<region_t, contract_t> > &latest_contracts =
        contracts_and_acks.at(latest_contracts_server_id).contracts;

    std::vector<std::pair<region_t, region_acks_t> > regions_and_values;
    regions_and_values.reserve(latest_contracts.size());
    for (const auto &latest_contract : latest_contracts) {
        region_acks_t server_acks;
        server_acks.latest_contract_id = latest_contract.first;
        server_acks.acks = {};
        regions_and_values.emplace_back(
            std::make_pair(latest_contract.second.first, std::move(server_acks)));
    }
    region_map_t<region_acks_t> regions(
        regions_and_values.begin(), regions_and_values.end());

    for (const auto &server : contracts_and_acks) {
        for (const auto &contract_ack : server.second.contract_acks) {
            auto contract_it = contracts.find(contract_ack.first);
            if (contract_it == contracts.end()) {
                /* When the executor is being reset we may receive acknowledgements for
                   contracts that are no longer in the set of all contracts. Ignoring
                   these will at worse result in a pessimistic status, which is fine
                   when this function is being used as part of `table_wait`. */
                continue;
            }
            region_map_t<region_acks_t> masked_regions =
                regions.mask(contract_it->second.get().first);
            for (auto &masked_region : masked_regions) {
                masked_region.second.acks.insert(
                    std::make_pair(server.first, contract_ack));
            }
            regions.update(masked_regions);
        }
    }

    *readiness_out = table_readiness_t::finished;
    std::vector<std::pair<region_t, shard_status_t> > foo;
    for (size_t i = 0; i < config_and_shards.shard_scheme.num_shards(); ++i) {
        region_t shard_region(config_and_shards.shard_scheme.get_shard_range(i));

        shard_status_t shard_status = calculate_shard_status(
            config_and_shards.config.shards.at(i),
            regions.mask(shard_region),
            contracts_and_acks,
            contracts);

        *readiness_out = std::min(*readiness_out, shard_status.readiness);
        foo.emplace_back(std::make_pair(shard_region, std::move(shard_status)));
    }
    if (shard_statuses_out != nullptr) {
        *shard_statuses_out = std::move(region_map_t<shard_status_t>(foo.begin(), foo.end()));
    }

    return true;
}
