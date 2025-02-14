#include "proxying_data_node_service_helpers.h"

#include <yt/yt/ytlib/chunk_client/job_spec_extensions.h>
#include <yt/yt/ytlib/chunk_client/data_slice_descriptor.h>

#include <yt/yt/ytlib/table_client/helpers.h>

#include <yt/yt/client/table_client/public.h>

#include <yt/yt/client/chunk_client/public.h>
#include <yt/yt/client/chunk_client/chunk_replica.h>

#include <yt/yt/core/misc/protobuf_helpers.h>

#include <yt/yt_proto/yt/client/chunk_client/proto/chunk_spec.pb.h>
#include <yt/yt_proto/yt/client/chunk_client/proto/chunk_meta.pb.h>

#include <yt/yt_proto/yt/client/table_chunk_format/proto/chunk_meta.pb.h>

namespace NYT::NExecNode {

using namespace NChunkClient;
using namespace NConcurrency;
using namespace NObjectClient;
using namespace NTableClient;

using namespace NControllerAgent::NProto;

using NObjectClient::TObjectId;

////////////////////////////////////////////////////////////////////////////////

static const NLogging::TLogger Logger("ProxyingDataNodeService");

////////////////////////////////////////////////////////////////////////////////

TChunkId MakeProxiedChunkId(TChunkId chunkId)
{
    return MakeId(
        EObjectType::Chunk,
        CellTagFromId(chunkId),
        CounterFromId(chunkId),
        EntropyFromId(chunkId));
}

bool CanJobUseProxyingDataNodeService(EJobType jobType)
{
    return jobType != NJobTrackerClient::EJobType::RemoteCopy;
}

void ModifyChunkSpecReplicas(
    NNodeTrackerClient::TNodeId nodeId,
    EJobType jobType,
    const std::vector<TTableSchemaPtr>& schemas,
    TTableInputSpec* tableSpec,
    THashMap<NChunkClient::TChunkId, TRefCountedChunkSpecPtr>* chunkSpecs)
{
    if (schemas.empty()) {
        return;
    }

    std::vector<NChunkClient::NProto::TChunkSpec> proxiedChunkSpecs;
    proxiedChunkSpecs.reserve(tableSpec->chunk_specs_size());

    for (auto& chunkSpec : *tableSpec->mutable_chunk_specs()) {
        if (!chunkSpec.use_proxying_data_node_service()) {
            continue;
        }

        auto tableIndex = chunkSpec.table_index();

        YT_VERIFY(std::ssize(schemas) > tableIndex);

        const auto& schema = schemas[tableIndex];
        auto chunkId = FromProto<TChunkId>(chunkSpec.chunk_id());

        // Skip tables with hunk columns.
        if (schema->HasHunkColumns() ||
            !IsBlobChunkId(chunkId) ||
            !CanJobUseProxyingDataNodeService(jobType))
        {
            chunkSpec.set_use_proxying_data_node_service(false);
            continue;
        }

        // 1. Chunks for which proxying is enabled are added to the new list - proxied_chunk_specs.
        //    For proxying chunks, the hostId, replicas, and chunkId are replaced.
        // 2. Chunks that are proxied are registered in the job input cache.
        // 3. Inside PatchProxiedChunkSpecs, proxying chunks replace chunks received in
        //    the scheduler and controller spec.
        // 4. JobProxy reads proxied chunks through replication reader via exe node, reads are cached in job input cache.
        // 5. For proxied chunks, the ChunkId always describes the EObjectType::Chunk type in order not to use
        //    the erasure reader in the job proxy. Using erasure reader for such chunks is incorrect,
        //    as it leads to incorrect reindexing of blocks.
        auto proxiedChunkId = MakeProxiedChunkId(chunkId);

        YT_LOG_INFO(
            "Modify chunk spec for job input cache (OldChunkId: %v, "
            "NewChunkId: %v, "
            "OldReplicaCount: %v, "
            "NewReplicaCount: %v)",
            chunkId,
            proxiedChunkId,
            chunkSpec.replicas_size(),
            1);

        {
            auto spec = New<TRefCountedChunkSpec>(chunkSpec);
            spec->set_use_proxying_data_node_service(false);
            chunkSpecs->emplace(
                proxiedChunkId,
                std::move(spec));
        }

        {
            NChunkClient::NProto::TChunkSpec proxiedChunkSpec;
            proxiedChunkSpec.CopyFrom(chunkSpec);

            auto newReplicas = TChunkReplicaWithMediumList();
            newReplicas.reserve(1);
            newReplicas.emplace_back(nodeId, 0, AllMediaIndex);

            ToProto(proxiedChunkSpec.mutable_chunk_id(), proxiedChunkId);
            proxiedChunkSpec.set_erasure_codec(ToProto<int>(NErasure::ECodec::None));
            ToProto(proxiedChunkSpec.mutable_replicas(), newReplicas);

            proxiedChunkSpecs.push_back(std::move(proxiedChunkSpec));
        }
    }

    ToProto(tableSpec->mutable_proxied_chunk_specs(), proxiedChunkSpecs);
}

void PatchInterruptDescriptor(
    const THashMap<NChunkClient::TChunkId, TRefCountedChunkSpecPtr> chunkIdToOriginalSpec,
    NChunkClient::TInterruptDescriptor& interruptDescriptor)
{
    auto updateChunkSpecsInDescriptorToOriginal = [&] (std::vector<TDataSliceDescriptor>& descriptors) {
        for (auto& descriptor : descriptors) {
            for (auto& chunkSpec : descriptor.ChunkSpecs) {
                auto chunkId = FromProto<TChunkId>(chunkSpec.chunk_id());
                if (auto originalSpecIt = chunkIdToOriginalSpec.find(chunkId)) {
                    auto originalSpec = originalSpecIt->second;

                    chunkSpec.mutable_chunk_id()->CopyFrom(originalSpec->chunk_id());
                    chunkSpec.set_erasure_codec(originalSpec->erasure_codec());
                    chunkSpec.mutable_replicas()->CopyFrom(originalSpec->replicas());
                }
            }
        }
    };

    updateChunkSpecsInDescriptorToOriginal(interruptDescriptor.UnreadDataSliceDescriptors);
    updateChunkSpecsInDescriptorToOriginal(interruptDescriptor.ReadDataSliceDescriptors);
}

THashMap<NChunkClient::TChunkId, TRefCountedChunkSpecPtr> ModifyChunkSpecForJobInputCache(
    NNodeTrackerClient::TNodeId nodeId,
    EJobType jobType,
    TJobSpecExt* jobSpecExt)
{
    auto dataSourceDirectoryExt = FindProtoExtension<NChunkClient::NProto::TDataSourceDirectoryExt>(jobSpecExt->extensions());
    std::vector<TTableSchemaPtr> schemas = GetJobInputTableSchemas(
        *jobSpecExt,
        dataSourceDirectoryExt ? FromProto<TDataSourceDirectoryPtr>(*dataSourceDirectoryExt) : nullptr);

    THashMap<NChunkClient::TChunkId, TRefCountedChunkSpecPtr> chunkSpecs;

    auto reconfigureTableSpecs = [&] (auto* tableSpecs) {
        for (auto& tableSpec : *tableSpecs) {
            ModifyChunkSpecReplicas(nodeId, jobType, schemas, &tableSpec, &chunkSpecs);
        }
    };

    reconfigureTableSpecs(jobSpecExt->mutable_input_table_specs());
    reconfigureTableSpecs(jobSpecExt->mutable_foreign_input_table_specs());

    return chunkSpecs;
}

THashMap<NChunkClient::TChunkId, TRefCountedChunkSpecPtr> PatchProxiedChunkSpecs(TJobSpec* jobSpecProto)
{
    THashMap<NChunkClient::TChunkId, TRefCountedChunkSpecPtr> chunkIdToOriginalSpec;
    auto* jobSpecExt = jobSpecProto->MutableExtension(TJobSpecExt::job_spec_ext);

    auto patchTableSpec = [&] (auto* tableSpec) {
        THashMap<TChunkId, NChunkClient::NProto::TChunkSpec> proxiedChunkSpecs;

        for (auto& chunkSpec : tableSpec->proxied_chunk_specs()) {
            proxiedChunkSpecs.emplace(
                FromProto<TChunkId>(chunkSpec.chunk_id()),
                std::move(chunkSpec));
        }

        std::vector<NChunkClient::NProto::TChunkSpec> newChunkSpecs;
        newChunkSpecs.reserve(tableSpec->chunk_specs_size());

        for (const auto& chunkSpec : tableSpec->chunk_specs()) {
            auto chunkId = FromProto<TChunkId>(chunkSpec.chunk_id());
            auto proxiedChunkId = MakeProxiedChunkId(chunkId);

            auto proxiedChunkSpecIt = proxiedChunkSpecs.find(proxiedChunkId);
            if (proxiedChunkSpecIt.IsEnd()) {
                auto newChunkSpec = chunkSpec;

                // For unpatched chunks, must explicitly set use_proxying_data_node_service = false.
                newChunkSpec.set_use_proxying_data_node_service(false);
                newChunkSpecs.push_back(newChunkSpec);
                chunkIdToOriginalSpec.emplace(chunkId, New<TRefCountedChunkSpec>(chunkSpec));
            } else {
                const auto& proxiedChunkSpec = proxiedChunkSpecIt->second;

                YT_VERIFY(proxiedChunkSpec.use_proxying_data_node_service());

                auto newChunkSpec = chunkSpec;

                newChunkSpec.mutable_chunk_id()->CopyFrom(proxiedChunkSpec.chunk_id());
                newChunkSpec.set_erasure_codec(proxiedChunkSpec.erasure_codec());
                newChunkSpec.mutable_replicas()->CopyFrom(proxiedChunkSpec.replicas());
                chunkIdToOriginalSpec.emplace(proxiedChunkId, New<TRefCountedChunkSpec>(chunkSpec));

                YT_LOG_INFO(
                    "Modify chunk spec for job input cache ("
                    "OldChunkId: %v, "
                    "NewChunkId: %v, "
                    "OldReplicaCount: %v, "
                    "NewReplicaCount: %v)",
                    chunkId,
                    proxiedChunkId,
                    chunkSpec.replicas_size(),
                    proxiedChunkSpec.replicas_size());

                newChunkSpecs.push_back(std::move(newChunkSpec));
            }
        }

        YT_VERIFY(tableSpec->chunk_specs_size() == std::ssize(newChunkSpecs));

        ToProto(tableSpec->mutable_chunk_specs(), std::move(newChunkSpecs));
    };

    auto patchTableSpecs = [&] (auto* tableSpecs) {
        for (auto& tableSpec : *tableSpecs) {
            patchTableSpec(&tableSpec);
        }
    };

    patchTableSpecs(jobSpecExt->mutable_input_table_specs());
    patchTableSpecs(jobSpecExt->mutable_foreign_input_table_specs());

    return chunkIdToOriginalSpec;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NExecNode
