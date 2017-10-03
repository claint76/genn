#pragma once

// GeNN includes
#include "base.h"

//----------------------------------------------------------------------------
// SynapseDynamicsKernel::Sparse
//----------------------------------------------------------------------------
namespace SynapseDynamicsKernel
{
class Sparse : public BaseStaticGrid
{
public:
    //------------------------------------------------------------------------
    // Kernel virtuals
    //------------------------------------------------------------------------
    //!< How compatible is this kernel generator with this synapse group?
    //!< Ascending values indicate compatibility and negative numbers indicate incompatible
    virtual int getCompatibility(const SynapseGroup &sg) const override;

    //------------------------------------------------------------------------
    // KernelGPU virtuals
    //------------------------------------------------------------------------
    //!< Get the name of the kernel (used to call it from runner)
    virtual std::string getKernelName() const override{ return "calcSparseSynapseDynamics"; }

protected:
    //------------------------------------------------------------------------
    // KernelGPU virtuals
    //------------------------------------------------------------------------
    //!< Determine how many threads this synapse group
    //!< requires, not taking into account block size etc
    virtual unsigned int getMaxNumThreads(const SynapseGroup &sg) const override;

    //------------------------------------------------------------------------
    // KernelGPUStaticGrid virtuals
    //------------------------------------------------------------------------
    virtual void generateGlobals(CodeStream &os, const std::string &ftype) const override;

    virtual void generateGroup(CodeStream &os, const SynapseGroup &sg, const std::string &ftype) const override;
};
}   // namespace SynapseDynamicsKernel