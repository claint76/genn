#include "modelSpecMerged.h"

// PLOG includes
#include <plog/Log.h>

// GeNN includes
#include "modelSpecInternal.h"

//----------------------------------------------------------------------------
// Anonymous namespace
//----------------------------------------------------------------------------
namespace
{
template<typename Group, typename MergedGroup, typename M>
void createMergedGroups(const std::map<std::string, Group> &groups, std::vector<MergedGroup> &mergedGroups, M canMerge)
{
    // Build temporary vector of references to groups
    std::vector<std::reference_wrapper<const Group>> unmergedGroups;
    unmergedGroups.reserve(groups.size());
    std::transform(groups.cbegin(), groups.cend(), std::back_inserter(unmergedGroups),
                    [](const typename std::map<std::string, Group>::value_type &g) { return std::cref(g.second); });

    // Loop through un-merged  groups
    while(!unmergedGroups.empty()) {
        // Remove last group from vector
        const Group &group = unmergedGroups.back().get();
        unmergedGroups.pop_back();

        // Start vector of groups that can be merged
        std::vector<std::reference_wrapper<const Group>> mergeTargets{group};

        // Loop through other remaining unmerged groups
        for(auto otherGroup = unmergedGroups.begin(); otherGroup != unmergedGroups.end();) {
            // If this 'other' group can be merged with original
            if(canMerge(group, otherGroup->get())) {
                LOGD << "\tMerging group '" << otherGroup->get().getName() << "' with '" << group.getName() << "'";

                // Add to list of merge targets
                mergeTargets.push_back(otherGroup->get());

                // Remove from unmerged list
                otherGroup = unmergedGroups.erase(otherGroup);
            }
            // Otherwise, advance to next group
            else {
                LOGD << "\tUnable to merge group '" << otherGroup->get().getName() << "' with '" << group.getName() << "'";
                ++otherGroup;
            }
        }

        // A new merged neuron group to model
        mergedGroups.emplace_back(mergedGroups.size(), mergeTargets);

    }
}
}   // Anonymous namespace

//----------------------------------------------------------------------------
// SynapseGroupMerged
//----------------------------------------------------------------------------
std::string SynapseGroupMerged::getPresynapticAxonalDelaySlot() const
{
    assert(getArchetype().getSrcNeuronGroup()->isDelayRequired());

    const unsigned int numDelaySteps = getArchetype().getDelaySteps();
    if(numDelaySteps == 0) {
        return "(*synapseGroup.srcSpkQuePtr)";
    }
    else {
        const unsigned int numSrcDelaySlots = getArchetype().getSrcNeuronGroup()->getNumDelaySlots();
        return "((*synapseGroup.srcSpkQuePtr + " + std::to_string(numSrcDelaySlots - numDelaySteps) + ") % " + std::to_string(numSrcDelaySlots) + ")";
    }
}
//----------------------------------------------------------------------------
std::string SynapseGroupMerged::getPostsynapticBackPropDelaySlot() const
{
    assert(getArchetype().getTrgNeuronGroup()->isDelayRequired());

    const unsigned int numBackPropDelaySteps = getArchetype().getBackPropDelaySteps();
    if(numBackPropDelaySteps == 0) {
        return "(*synapseGroup.trgSpkQuePtr)";
    }
    else {
        const unsigned int numTrgDelaySlots = getArchetype().getTrgNeuronGroup()->getNumDelaySlots();
        return "((*synapseGroup.trgSpkQuePtr + " + std::to_string(numTrgDelaySlots - numBackPropDelaySteps) + ") % " + std::to_string(numTrgDelaySlots) + ")";
    }
}
//----------------------------------------------------------------------------
std::string SynapseGroupMerged::getDendriticDelayOffset(const std::string &offset) const
{
    assert(getArchetype().isDendriticDelayRequired());

    if(offset.empty()) {
        return "(*synapseGroup.denDelayPtr * synapseGroup.numTrgNeurons) + ";
    }
    else {
        return "(((*synapseGroup.denDelayPtr + " + offset + ") % " + std::to_string(getArchetype().getMaxDendriticDelayTimesteps()) + ") * synapseGroup.numTrgNeurons) + ";
    }
}


//----------------------------------------------------------------------------
// ModelSpecMerged
//----------------------------------------------------------------------------
ModelSpecMerged::ModelSpecMerged(const ModelSpecInternal &model)
    : m_Model(model)
{
    LOGD << "Merging neuron groups:";
    createMergedGroups(model.getLocalNeuronGroups(), m_MergedLocalNeuronGroups,
                       [](const NeuronGroupInternal &a, const NeuronGroupInternal &b){ return a.canBeMerged(b); });

    LOGD << "Merging synapse groups:";
    createMergedGroups(model.getLocalSynapseGroups(), m_MergedLocalSynapseGroups,
                       [](const SynapseGroupInternal &a, const SynapseGroupInternal &b){ return a.canWUBeMerged(b); });
}