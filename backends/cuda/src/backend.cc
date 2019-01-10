#include "backend.h"

// Standard C++ includes
#include <algorithm>
#include <iostream>
#include <numeric>

// CUDA includes
#include <cuda.h>
#include <cuda_runtime.h>

// PLOG includes
#include <plog/Log.h>

// Filesystem includes
#include "path.h"

// GeNN includes
#include "codeGenUtils.h"
#include "codeStream.h"
#include "modelSpec.h"

// GeNN code generator includes
#include "code_generator/substitutions.h"


#if CUDA_VERSION >= 6050
#define CHECK_CU_ERRORS(call)                                        \
  {                                                                \
    CUresult error = call;                                        \
    if (error != CUDA_SUCCESS)                                        \
      {                                                                \
        const char *errStr;                                        \
        cuGetErrorName(error, &errStr);                                \
        std::cerr << __FILE__ << ": " <<  __LINE__;                        \
        std::cerr << ": cuda driver error " << error << ": ";        \
        std::cerr << errStr << std::endl;                                        \
        exit(EXIT_FAILURE);                                        \
      }                                                                \
  }
#else
#define CHECK_CU_ERRORS(call) call
#endif

#define CHECK_CUDA_ERRORS(call)                                        \
  {                                                                \
    cudaError_t error = call;                                        \
    if (error != cudaSuccess)                                        \
      {                                                                \
        std::cerr << __FILE__ << ": " <<  __LINE__;                        \
        std::cerr << ": cuda runtime error " << error << ": ";        \
        std::cerr << cudaGetErrorString(error) << std::endl;                \
        exit(EXIT_FAILURE);                                        \
      }                                                                \
  }

//--------------------------------------------------------------------------
// Anonymous namespace
//--------------------------------------------------------------------------
namespace
{
typedef std::map<unsigned int, std::pair<bool, size_t>> KernelOptimisationOutput;

const std::vector<FunctionTemplate> cudaFunctions = {
    {"gennrand_uniform", 0, "curand_uniform_double($(rng))", "curand_uniform($(rng))"},
    {"gennrand_normal", 0, "curand_normal_double($(rng))", "curand_normal($(rng))"},
    {"gennrand_exponential", 0, "exponentialDistDouble($(rng))", "exponentialDistFloat($(rng))"},
    {"gennrand_log_normal", 2, "curand_log_normal_double($(rng), $(0), $(1))", "curand_log_normal_float($(rng), $(0), $(1))"},
    {"gennrand_gamma", 1, "gammaDistDouble($(rng), $(0))", "gammaDistFloat($(rng), $(0))"}
};

size_t ceilDivide(size_t numerator, size_t denominator)
{
    return ((numerator + denominator - 1) / denominator);
}
//--------------------------------------------------------------------------
size_t padSize(size_t size, size_t blockSize)
{
    return ceilDivide(size, blockSize) * blockSize;
}
//--------------------------------------------------------------------------
bool canPushPullVar(VarLocation loc)
{
    // A variable can be pushed and pulled if it is located
    // on both host and device and doesn't use zero-copy memory
    return ((loc & VarLocation::HOST) &&
            (loc & VarLocation::DEVICE) &&
            !(loc & VarLocation::ZERO_COPY));
}
//--------------------------------------------------------------------------
void getDeviceArchitectureProperties(const cudaDeviceProp &deviceProps, size_t &warpAllocGran, size_t &regAllocGran,
                                     size_t &smemAllocGran, size_t &maxBlocksPerSM)
{
    if(deviceProps.major == 1) {
        smemAllocGran = 512;
        warpAllocGran = 2;
        regAllocGran = (deviceProps.minor < 2) ? 256 : 512;
        maxBlocksPerSM = 8;
    }
    else if(deviceProps.major == 2) {
        smemAllocGran = 128;
        warpAllocGran = 2;
        regAllocGran = 64;
        maxBlocksPerSM = 8;
    }
    else if(deviceProps.major == 3) {
        smemAllocGran = 256;
        warpAllocGran = 4;
        regAllocGran = 256;
        maxBlocksPerSM = 16;
    }
    else if(deviceProps.major == 5) {
        smemAllocGran = 256;
        warpAllocGran = 4;
        regAllocGran = 256;
        maxBlocksPerSM = 32;
    }
    else if(deviceProps.major == 6) {
        smemAllocGran = 256;
        warpAllocGran = (deviceProps.minor == 0) ? 2 : 4;
        regAllocGran = 256;
        maxBlocksPerSM = 32;
    }
    else {
        smemAllocGran = 256;
        warpAllocGran = 4;
        regAllocGran = 256;
        maxBlocksPerSM = 32;

        if(deviceProps.major > 7) {
            LOGW << "Unsupported CUDA device major version: " << deviceProps.major;
            LOGW << "This is a bug! Please report it at https://github.com/genn-team/genn.";
            LOGW << "Falling back to next latest SM version parameters.";
        }
    }
}
//--------------------------------------------------------------------------
void calcGroupSizes(const NNmodel &model, std::vector<unsigned int> (&groupSizes)[CodeGenerator::Backends::CUDA::KernelMax])
{
    using namespace CodeGenerator;
    using namespace Backends;

    // **TODO** this belongs in code generator somewhere

    // Loop through neuron groups
    for(const auto &n : model.getLocalNeuronGroups()) {
        // Add number of neurons to vector of neuron kernels
        groupSizes[CUDA::KernelNeuronUpdate].push_back(n.second.getNumNeurons());

        // If this neuron group requires on-device initialisation
        if(n.second.isSimRNGRequired() || n.second.isInitCodeRequired()) {
            groupSizes[CUDA::KernelInitialize].push_back(n.second.getNumNeurons());
        }
    }

    // Loop through synapse groups
    for(const auto &s : model.getLocalSynapseGroups()) {
        groupSizes[CUDA::KernelPresynapticUpdate].push_back(CUDA::getNumPresynapticUpdateThreads(s.second));

        if(!s.second.getWUModel()->getLearnPostCode().empty()) {
            groupSizes[CUDA::KernelPostsynapticUpdate].push_back(CUDA::getNumPostsynapticUpdateThreads(s.second));
        }

        if (!s.second.getWUModel()->getLearnPostCode().empty()) {
            groupSizes[CUDA::KernelSynapseDynamicsUpdate].push_back(CUDA::getNumSynapseDynamicsThreads(s.second));
        }

        // If synapse group has individual weights and needs device initialisation
        if((s.second.getMatrixType() & SynapseMatrixWeight::INDIVIDUAL) && s.second.isWUVarInitRequired()) {
            const unsigned int numSrcNeurons = s.second.getSrcNeuronGroup()->getNumNeurons();
            const unsigned int numTrgNeurons = s.second.getTrgNeuronGroup()->getNumNeurons();
            if(s.second.getMatrixType() & SynapseMatrixConnectivity::SPARSE) {
                groupSizes[CUDA::KernelInitializeSparse].push_back(numSrcNeurons);
            }
            else {
                groupSizes[CUDA::KernelInitialize].push_back(numSrcNeurons * numTrgNeurons);
            }
        }
    }

    // Add group sizes for reset kernels
    groupSizes[CUDA::KernelPreNeuronReset].push_back(model.getLocalNeuronGroups().size());
    groupSizes[CUDA::KernelPreSynapseReset].push_back(model.getNumPreSynapseResetRequiredGroups());
}
//--------------------------------------------------------------------------
KernelOptimisationOutput optimizeBlockSize(int deviceID, const NNmodel &model, CodeGenerator::Backends::CUDA::KernelBlockSize &blockSize, const CodeGenerator::Backends::CUDA::Preferences &preferences, 
                                           CodeGenerator::Backends::Base::Generator generator, const filesystem::path &outputPath)
{
    using namespace CodeGenerator;
    using namespace Backends;

    // Calculate model group sizes
    std::vector<unsigned int> groupSizes[CUDA::KernelMax];
    calcGroupSizes(model, groupSizes);

    // Create CUDA drive API device and context for accessing kernel attributes
    CUdevice cuDevice;
    CUcontext cuContext;
    CHECK_CU_ERRORS(cuDeviceGet(&cuDevice, deviceID));
    CHECK_CU_ERRORS(cuCtxCreate(&cuContext, 0, cuDevice));

    // Bitset to mark which kernels are present and array of their attributes for each repetition
    cudaFuncAttributes krnlAttr[2][CUDA::KernelMax];

    // Do two repititions with different candidate kernel size
    const size_t warpSize = 32;
    const size_t repBlockSizes[2] = {warpSize, warpSize * 2};
    KernelOptimisationOutput kernelsToOptimise;
    for(unsigned int r = 0; r < 2; r++) {
        LOGD  << "Generating code with block size:" << repBlockSizes[r];

        // Start with all group sizes set to warp size
        std::fill(blockSize.begin(), blockSize.end(), repBlockSizes[r]);

        // Create backend
        CUDA backend(blockSize, preferences, 0, deviceID);

        // Generate code
        const auto moduleNames = generator(model, backend, outputPath);

        // Set context
        // **NOTE** CUDA calls in code generation seem to lose driver context
        CHECK_CU_ERRORS(cuCtxSetCurrent(cuContext));

        // Loop through generated modules
        for(const auto &m : moduleNames) {
            // Build module
            const std::string modulePath = (outputPath / m).str();
            
            const std::string nvccCommand = "nvcc -cubin " + backend.getNVCCFlags() + " -o " + modulePath + ".cubin " + modulePath + ".cc";
            if(system(nvccCommand.c_str()) != 0) {
                throw std::runtime_error("optimizeBlockSize: NVCC failed");
            }

            // Load compiled module
            CUmodule module;
            CHECK_CU_ERRORS(cuModuleLoad(&module, (modulePath + ".cubin").c_str()));

            // Loop through kernels
            for (unsigned int k = 0; k < CUDA::KernelMax; k++) {
                // If function is found
                CUfunction kern;
                CUresult res = cuModuleGetFunction(&kern, module, CUDA::KernelNames[k]);
                if (res == CUDA_SUCCESS) {
                    LOGD << "\tKernel '" << CUDA::KernelNames[k] << "' found";

                    // Read it's attributes and add blank entry to map of kernels to optimise
                    cudaFuncGetAttributes(&krnlAttr[r][k], kern);
                    kernelsToOptimise.emplace(std::piecewise_construct,
                                              std::forward_as_tuple(k),
                                              std::forward_as_tuple(false, 0));

                    LOGD << "\t\tShared memory bytes:" << krnlAttr[r][k].sharedSizeBytes;
                    LOGD << "\t\tNum registers:" << krnlAttr[r][k].numRegs;
                }
            }

            // Unload module
            CHECK_CU_ERRORS(cuModuleUnload(module));
        }
    }

    // Destroy context
    CHECK_CU_ERRORS(cuCtxDestroy(cuContext));

    // Get device properties
    cudaDeviceProp deviceProps;
    CHECK_CUDA_ERRORS(cudaGetDeviceProperties(&deviceProps, deviceID));

    // Get properties of device architecture
    size_t warpAllocGran;
    size_t regAllocGran;
    size_t smemAllocGran;
    size_t maxBlocksPerSM;
    getDeviceArchitectureProperties(deviceProps, warpAllocGran, regAllocGran, smemAllocGran, maxBlocksPerSM);

    // Zero block sizes
    std::fill(blockSize.begin(), blockSize.end(), 0);

    // Loop through kernels to optimise
    for(auto &k : kernelsToOptimise) {
        LOGD << "Kernel '" << CUDA::KernelNames[k.first] << "':";

        // Get required number of registers and shared memory bytes for this kernel
        // **NOTE** register requirements are assumed to remain constant as they're vector-width
        const size_t reqNumRegs = krnlAttr[0][k.first].numRegs;
        const size_t reqSharedMemBytes[2] = {krnlAttr[0][k.first].sharedSizeBytes, krnlAttr[1][k.first].sharedSizeBytes};

        // Calculate coefficients for requiredSharedMemBytes = (A * blockThreads) + B model
        const size_t reqSharedMemBytesA = (reqSharedMemBytes[1] - reqSharedMemBytes[0]) / (repBlockSizes[1] - repBlockSizes[0]);
        const size_t reqSharedMemBytesB = reqSharedMemBytes[0] - (reqSharedMemBytesA * repBlockSizes[0]);

        // Loop through possible
        const size_t maxBlockWarps = deviceProps.maxThreadsPerBlock / warpSize;
        for(size_t blockWarps = 1; blockWarps < maxBlockWarps; blockWarps++) {
            const size_t blockThreads = blockWarps * warpSize;
            LOGD << "\tCandidate block size:" << blockThreads;

            // Estimate shared memory for block size and padd
            const size_t reqSharedMemBytes = padSize((reqSharedMemBytesA * blockThreads) + reqSharedMemBytesB, smemAllocGran);
            LOGD << "\t\tEstimated shared memory required:" << reqSharedMemBytes << " bytes (padded)";

            // Calculate number of blocks the groups used by this kernel will require
            const size_t reqBlocks = std::accumulate(groupSizes[k.first].begin(), groupSizes[k.first].end(), 0,
                                                        [blockThreads](size_t acc, size_t size)
                                                        {
                                                            return acc + ceilDivide(size, blockThreads);
                                                        });
            LOGD << "\t\tBlocks required (according to padded sum):" << reqBlocks;

            // Start estimating SM block limit - the number of blocks of this size that can run on a single SM
            size_t smBlockLimit = deviceProps.maxThreadsPerMultiProcessor / blockThreads;
            LOGD << "\t\tSM block limit due to maxThreadsPerMultiProcessor:" << smBlockLimit;

            smBlockLimit = std::min(smBlockLimit, maxBlocksPerSM);
            LOGD << "\t\tSM block limit corrected for maxBlocksPerSM:" << smBlockLimit;

            // If register allocation is per-block
            if (deviceProps.major == 1) {
                // Pad size of block based on warp allocation granularity
                const size_t paddedNumBlockWarps = padSize(blockWarps, warpAllocGran);

                // Calculate number of registers per block and pad with register allocation granularity
                const size_t paddedNumRegPerBlock = padSize(paddedNumBlockWarps * reqNumRegs * warpSize, regAllocGran);

                // Update limit based on maximum registers per block
                // **NOTE** this doesn't quite make sense either
                smBlockLimit = std::min(smBlockLimit, deviceProps.regsPerBlock / paddedNumRegPerBlock);
            }
            // Otherwise, if register allocation is per-warp
            else {
                // Caculate number of registers per warp and pad with register allocation granularity
                const size_t paddedNumRegPerWarp = padSize(reqNumRegs * warpSize, regAllocGran);

                // **THINK** I don't understand this
                //blockLimit = floor(deviceProps.regsPerBlock / (paddedNumRegPerWarp * warpAllocGran)*warpAllocGran;

                // **NOTE** this doesn't quite make sense either
                //smBlockLimit = std::min(smBlockLimit, blockLimit / blockWarps);
            }
            LOGD << "\t\tSM block limit corrected for registers:" << smBlockLimit;

            // If this kernel requires any shared memory, update limit to reflect shared memory available in each multiprocessor
            // **NOTE** this used to be sharedMemPerBlock but that seems incorrect
            if(reqSharedMemBytes != 0) {
                smBlockLimit = std::min(smBlockLimit, deviceProps.sharedMemPerMultiprocessor / reqSharedMemBytes);
                LOGD << "\t\tSM block limit corrected for shared memory:" << smBlockLimit;
            }

            // Calculate occupancy
            const size_t newOccupancy = blockWarps * smBlockLimit * deviceProps.multiProcessorCount;

            // Use a small block size if it allows all groups to occupy the device concurrently
            if (reqBlocks <= (smBlockLimit * deviceProps.multiProcessorCount)) {
                blockSize[k.first] = blockThreads;
                k.second.second = newOccupancy;
                k.second.first = true;

                LOGD << "\t\tSmall model situation detected - block size:" << blockSize[k.first];

                // For small model the first (smallest) block size allowing it is chosen
                break;
            }
            // Otherwise, if we've improved on previous best occupancy
            else if(newOccupancy > k.second.second) {
                blockSize[k.first] = blockThreads;
                k.second.second = newOccupancy;

                LOGD << "\t\tNew highest occupancy: " << newOccupancy << ", block size:" << blockSize[k.first];
            }

        }

        LOGI << "Kernel: " << CUDA::KernelNames[k.first] << ", block size:" << blockSize[k.first];
    }

    // Return optimisation data
    return kernelsToOptimise;
}
//--------------------------------------------------------------------------
int chooseOptimalDevice(const NNmodel &model, CodeGenerator::Backends::CUDA::KernelBlockSize &blockSize, const CodeGenerator::Backends::CUDA::Preferences &preferences, 
                        CodeGenerator::Backends::Base::Generator generator, const filesystem::path &outputPath)
{
    using namespace CodeGenerator;
    using namespace Backends;
    
    // Get number of devices
    int deviceCount;
    CHECK_CUDA_ERRORS(cudaGetDeviceCount(&deviceCount));
    if(deviceCount == 0) {
        throw std::runtime_error("No CUDA devices found");
    }

    // Loop through devices
    typedef std::tuple<int, size_t, size_t, CUDA::KernelBlockSize> Device;
    std::vector<Device> devices;
    devices.reserve(deviceCount);
    for(int d = 0; d < deviceCount; d++) {
        // Get properties
        cudaDeviceProp deviceProps;
        CHECK_CUDA_ERRORS(cudaGetDeviceProperties(&deviceProps, d));
        const int smVersion = (deviceProps.major * 10) + deviceProps.minor;

        // Optimise block size for this device
        CUDA::KernelBlockSize optimalBlockSize;
        const auto kernels = optimizeBlockSize(d, model, optimalBlockSize, preferences, generator, outputPath);

        // Sum up occupancy of each kernel
        const size_t totalOccupancy = std::accumulate(kernels.begin(), kernels.end(), 0,
                                                      [](size_t acc, const KernelOptimisationOutput::value_type &kernel)
                                                      {
                                                          return acc + kernel.second.second;
                                                      });

        // Count number of kernels that count as small models
        const size_t numSmallModelKernels = std::accumulate(kernels.begin(), kernels.end(), 0,
                                                            [](size_t acc, const KernelOptimisationOutput::value_type &kernel)
                                                            {
                                                                return acc + (kernel.second.first ? 1 : 0);
                                                            });

        LOGD << "Device " << d << " - total occupancy:" << totalOccupancy << ", number of small models:" << numSmallModelKernels << ", SM version:" << smVersion;
        devices.emplace_back(smVersion, totalOccupancy, numSmallModelKernels, optimalBlockSize);
    }

    // Find best device
    const auto bestDevice = std::min_element(devices.cbegin(), devices.cend(),
        [](const Device &a, const Device &b)
        {
            // If 'a' have a higher number of small model kernels -  return true - it is better
            const size_t numSmallModelKernelsA = std::get<2>(a);
            const size_t numSmallModelKernelsB = std::get<2>(b);
            if (numSmallModelKernelsA > numSmallModelKernelsB) {
                return true;
            }
            // Otherwise, if the two devices have an identical small model kernel count
            else if(numSmallModelKernelsA == numSmallModelKernelsB) {
                // If 'a' has a higher total occupancy - return true - it is better
                const size_t totalOccupancyA = std::get<1>(a);
                const size_t totalOccupancyB = std::get<1>(b);
                if(totalOccupancyA > totalOccupancyB) {
                    return true;
                }
                // Otherwise, if the two devices have identical occupancy
                else if(totalOccupancyA == totalOccupancyB) {
                    // If 'a' has a higher SM version - return true - it's better
                    const int smVersionA = std::get<0>(a);
                    const int smVersionB = std::get<0>(b);
                    if(smVersionA > smVersionB) {
                        return true;
                    }
                }
            }

            // 'a' is not better - return false
            return false;
        });

    // Find ID of best device
    const int bestDeviceID = std::distance(devices.cbegin(), bestDevice);
    LOGI << "Optimal  device " << bestDeviceID << " - total occupancy:" << std::get<1>(*bestDevice) << ", number of small models:" << std::get<2>(*bestDevice) << ", SM version:" << std::get<0>(*bestDevice);

    // Get optimal block size from best device
    blockSize = std::get<3>(*bestDevice);


    // Return ID of best device
    return bestDeviceID;
}
//--------------------------------------------------------------------------
int chooseDeviceWithMostGlobalMemory()
{
    // Get number of devices
    int deviceCount;
    CHECK_CUDA_ERRORS(cudaGetDeviceCount(&deviceCount));
    if(deviceCount == 0) {
        throw std::runtime_error("No CUDA devices found");
    }

    // Loop through devices
    size_t mostGlobalMemory = 0;
    int bestDevice = -1;
    for(int d = 0; d < deviceCount; d++) {
        // Get properties
        cudaDeviceProp deviceProps;
        CHECK_CUDA_ERRORS(cudaGetDeviceProperties(&deviceProps, d));

        // If this device improves on previous best
        if(deviceProps.totalGlobalMem > mostGlobalMemory) {
            mostGlobalMemory = deviceProps.totalGlobalMem;
            bestDevice = d;
        }
    }

    LOGI << "Using device " << bestDevice << " which has " << mostGlobalMemory << " bytes of global memory";
    return bestDevice;
}
}   // Anonymous namespace

//--------------------------------------------------------------------------
// CodeGenerator::Backends::CUDA
//--------------------------------------------------------------------------
namespace CodeGenerator
{
namespace Backends
{
const char *CUDA::KernelNames[KernelMax] = {
    "updateNeuronsKernel",
    "updatePresynapticKernel",
    "updatePostsynapticKernel",
    "updateSynapseDynamicsKernel",
    "initializeKernel",
    "initializeSparseKernel",
    "preNeuronResetKernel",
    "preSynapseResetKernel"};
//--------------------------------------------------------------------------
CUDA::CUDA(const KernelBlockSize &kernelBlockSizes, const Preferences &preferences, int localHostID, int device)
:   m_KernelBlockSizes(kernelBlockSizes), m_Preferences(preferences), m_LocalHostID(localHostID), m_ChosenDeviceID(device)
{
    // Set device
    CHECK_CUDA_ERRORS(cudaSetDevice(device));

    // Get device properties
    CHECK_CUDA_ERRORS(cudaGetDeviceProperties(&m_ChosenDevice, device));

    // Get CUDA runtime version
    cudaRuntimeGetVersion(&m_RuntimeVersion);
}
//--------------------------------------------------------------------------
void CUDA::genNeuronUpdate(CodeStream &os, const NNmodel &model, NeuronGroupHandler handler) const
{
    // Generate reset kernel to be run before the neuron kernel
    size_t idPreNeuronReset = 0;
    os << "extern \"C\" __global__ void " << KernelNames[KernelPreNeuronReset] << "()";
    {
        CodeStream::Scope b(os);

        os << "unsigned int id = " << m_KernelBlockSizes[KernelPreNeuronReset] << " * blockIdx.x + threadIdx.x;" << std::endl;

        // Loop through remote neuron groups
        for(const auto &n : model.getRemoteNeuronGroups()) {
            if(n.second.hasOutputToHost(m_LocalHostID) && n.second.isDelayRequired()) {
                if(idPreNeuronReset > 0) {
                    os << "else ";
                }
                os << "if(id == " << (idPreNeuronReset++) << ")";
                {
                    CodeStream::Scope b(os);
                    os << "dd_spkQuePtr" << n.first << " = (dd_spkQuePtr" << n.first << " + 1) % " << n.second.getNumDelaySlots() << ";" << std::endl;
                }
            }
        }

        // Loop through local neuron groups
        for(const auto &n : model.getLocalNeuronGroups()) {
            if(idPreNeuronReset > 0) {
                os << "else ";
            }
            os << "if(id == " << (idPreNeuronReset++) << ")";
            {
                CodeStream::Scope b(os);

                if (n.second.isDelayRequired()) { // with delay
                    os << "dd_spkQuePtr" << n.first << " = (dd_spkQuePtr" << n.first << " + 1) % " << n.second.getNumDelaySlots() << ";" << std::endl;

                    if (n.second.isSpikeEventRequired()) {
                        os << "dd_glbSpkCntEvnt" << n.first << "[dd_spkQuePtr" << n.first << "] = 0;" << std::endl;
                    }
                    if (n.second.isTrueSpikeRequired()) {
                        os << "dd_glbSpkCnt" << n.first << "[dd_spkQuePtr" << n.first << "] = 0;" << std::endl;
                    }
                    else {
                        os << "dd_glbSpkCnt" << n.first << "[0] = 0;" << std::endl;
                    }
                }
                else { // no delay
                    if (n.second.isSpikeEventRequired()) {
                        os << "dd_glbSpkCntEvnt" << n.first << "[0] = 0;" << std::endl;
                    }
                    os << "dd_glbSpkCnt" << n.first << "[0] = 0;" << std::endl;
                }
            }
        }
    }

    /*
    for(const auto &n : model.getLocalNeuronGroups()) {*/
    size_t idStart = 0;
    os << "extern \"C\" __global__ void " << KernelNames[KernelNeuronUpdate] << "(";
    for(const auto &p : model.getNeuronKernelParameters()) {
        os << p.second << " " << p.first << ", ";
    }
    for(const auto &p : model.getCurrentSourceKernelParameters()) {
        os << p.second << " " << p.first << ", ";
    }
    os << model.getTimePrecision() << " t)" << std::endl;
    {
        CodeStream::Scope b(os);
        os << "const unsigned int id = " << m_KernelBlockSizes[KernelNeuronUpdate] << " * blockIdx.x + threadIdx.x; " << std::endl;

        Substitutions kernelSubs(cudaFunctions);
        kernelSubs.addVarSubstitution("t", "t");

        // If any neuron groups emit spike events
        if(std::any_of(model.getLocalNeuronGroups().cbegin(), model.getLocalNeuronGroups().cend(),
            [](const NNmodel::NeuronGroupValueType &n){ return n.second.isSpikeEventRequired(); }))
        {
            os << "__shared__ volatile unsigned int shSpkEvnt[" << m_KernelBlockSizes[KernelNeuronUpdate] << "];" << std::endl;
            os << "__shared__ volatile unsigned int shPosSpkEvnt;" << std::endl;
            os << "__shared__ volatile unsigned int shSpkEvntCount;" << std::endl;
            os << std::endl;
            os << "if (threadIdx.x == 1);";
            {
                CodeStream::Scope b(os);
                os << "shSpkEvntCount = 0;" << std::endl;
            }
            os << std::endl;
        }

        // If any neuron groups emit true spikes
        if(std::any_of(model.getLocalNeuronGroups().cbegin(), model.getLocalNeuronGroups().cend(),
            [](const NNmodel::NeuronGroupValueType &n){ return !n.second.getNeuronModel()->getThresholdConditionCode().empty(); }))
        {
            os << "__shared__ volatile unsigned int shSpk[" << m_KernelBlockSizes[KernelNeuronUpdate] << "];" << std::endl;
            os << "__shared__ volatile unsigned int shPosSpk;" << std::endl;
            os << "__shared__ volatile unsigned int shSpkCount;" << std::endl;
            os << "if (threadIdx.x == 0);";
            {
                CodeStream::Scope b(os);
                os << "shSpkCount = 0;" << std::endl;
            }
            os << std::endl;
        }
            
        os << "__syncthreads();" << std::endl;

        // Parallelise over neuron groups
        genParallelGroup<NeuronGroup>(os, kernelSubs, model.getLocalNeuronGroups(), idStart,
            [this](const NeuronGroup &ng){ return padSize(ng.getNumNeurons(), m_KernelBlockSizes[KernelNeuronUpdate]); },
            [&model, handler, this](CodeStream &os, const NeuronGroup &ng, Substitutions &popSubs)
            {
                // Get name of rng to use for this neuron
                popSubs.addVarSubstitution("rng", "&dd_rng" + ng.getName() + "[" + popSubs.getVarSubstitution("id") + "]");
                
                // Call handler to generate generic neuron code
                os << "if(" << popSubs.getVarSubstitution("id") << " < " << ng.getNumNeurons() << ")";
                {
                    CodeStream::Scope b(os);
                    handler(os, ng, popSubs);
                }

                os << "__syncthreads();" << std::endl;

                if (ng.isSpikeEventRequired()) {
                    os << "if (threadIdx.x == 1)";
                    {
                        CodeStream::Scope b(os);
                        os << "if (shSpkEvntCount > 0)";
                        {
                            CodeStream::Scope b(os);
                            os << "shPosSpkEvnt = atomicAdd((unsigned int *) &dd_glbSpkCntEvnt" << ng.getName();
                            if (ng.isDelayRequired()) {
                                os << "[dd_spkQuePtr" << ng.getName() << "], shSpkEvntCount);" << std::endl;
                            }
                            else {
                                os << "[0], shSpkEvntCount);" << std::endl;
                            }
                        }
                    } // end if (threadIdx.x == 0)
                    os << "__syncthreads();" << std::endl;
                }

                if (!ng.getNeuronModel()->getThresholdConditionCode().empty()) {
                    os << "if (threadIdx.x == 0)";
                    {
                        CodeStream::Scope b(os);
                        os << "if (shSpkCount > 0)";
                        {
                            CodeStream::Scope b(os);
                            os << "shPosSpk = atomicAdd((unsigned int *) &dd_glbSpkCnt" << ng.getName();
                            if (ng.isDelayRequired() && ng.isTrueSpikeRequired()) {
                                os << "[dd_spkQuePtr" << ng.getName() << "], shSpkCount);" << std::endl;
                            }
                            else {
                                os << "[0], shSpkCount);" << std::endl;
                            }
                        }
                    } // end if (threadIdx.x == 1)
                    os << "__syncthreads();" << std::endl;
                }

                const std::string queueOffset = ng.isDelayRequired() ? "writeDelayOffset + " : "";
                if (ng.isSpikeEventRequired()) {
                    os << "if (threadIdx.x < shSpkEvntCount)";
                    {
                        CodeStream::Scope b(os);
                        os << "dd_glbSpkEvnt" << ng.getName() << "[" << queueOffset << "shPosSpkEvnt + threadIdx.x] = shSpkEvnt[threadIdx.x];" << std::endl;
                    }
                }

                if (!ng.getNeuronModel()->getThresholdConditionCode().empty()) {
                    const std::string queueOffsetTrueSpk = ng.isTrueSpikeRequired() ? queueOffset : "";

                    os << "if (threadIdx.x < shSpkCount)";
                    {
                        CodeStream::Scope b(os);
                        os << "dd_glbSpk" << ng.getName() << "[" << queueOffsetTrueSpk << "shPosSpk + threadIdx.x] = shSpk[threadIdx.x];" << std::endl;
                        if (ng.isSpikeTimeRequired()) {
                            os << "dd_sT" << ng.getName() << "[" << queueOffset << "shSpk[threadIdx.x]] = t;" << std::endl;
                        }
                    }
                }
            }
        );
    }

    os << "void updateNeurons(" << model.getTimePrecision() << ")";
    {
        CodeStream::Scope b(os);
        if(idPreNeuronReset > 0) {
            CodeStream::Scope b(os);
            genKernelDimensions(os, KernelPreNeuronReset, idPreNeuronReset);
            os << KernelNames[KernelPreNeuronReset] << "<<<grid, threads>>>();" << std::endl;
        }
        if(idStart > 0) {
            CodeStream::Scope b(os);
            genKernelDimensions(os, KernelNeuronUpdate, idStart);
            os << KernelNames[KernelNeuronUpdate] << "<<<grid, threads>>>(";
            for(const auto &p : model.getNeuronKernelParameters()) {
                os << p.first << ", ";
            }
            for(const auto &p : model.getCurrentSourceKernelParameters()) {
                os << p.first << ", ";
            }
            os << "t);" << std::endl;
        }
    }
}
//--------------------------------------------------------------------------
void CUDA::genSynapseUpdate(CodeStream &os, const NNmodel &model,
                            SynapseGroupHandler wumThreshHandler, SynapseGroupHandler wumSimHandler,
                            SynapseGroupHandler postLearnHandler, SynapseGroupHandler synapseDynamicsHandler) const
{
    // If a reset kernel is required to be run before the synapse kernel
    size_t idPreSynapseReset = 0;
    if(model.isPreSynapseResetRequired())
    {
        // pre synapse reset kernel header
        os << "extern \"C\" __global__ void " << KernelNames[KernelPreSynapseReset] << "()";
        {
            CodeStream::Scope b(os);

            os << "unsigned int id = " << m_KernelBlockSizes[KernelPreSynapseReset] << " * blockIdx.x + threadIdx.x;" << std::endl;

            // Loop through neuron groups
            unsigned int groupID = 0;
            for(const auto &n : model.getLocalNeuronGroups()) {
                // Loop through incoming synaptic populations
                for(const auto &m : n.second.getMergedInSyn()) {
                    const auto *sg = m.first;

                     // If this kernel requires dendritic delay
                    if(sg->isDendriticDelayRequired()) {
                        if(groupID > 0) {
                            os << "else ";
                        }
                        os << "if(id == " << (groupID++) << ")";
                        {
                            CodeStream::Scope b(os);

                            os << "dd_denDelayPtr" << sg->getPSModelTargetName() << " = (dd_denDelayPtr" << sg->getPSModelTargetName() << " + 1) % " << sg->getMaxDendriticDelayTimesteps() << ";" << std::endl;
                        }
                    }
                }
            }
        }
    }

    size_t idPresynapticStart = 0;
    os << "extern \"C\" __global__ void " << KernelNames[KernelPresynapticUpdate] << "(";
    for (const auto &p : model.getSynapseKernelParameters()) {
        os << p.second << " " << p.first << ", ";
    }
    os << model.getTimePrecision() << " t)" << std::endl; // end of synapse kernel header
    {
        CodeStream::Scope b(os);
        
        Substitutions kernelSubs(cudaFunctions);
        kernelSubs.addVarSubstitution("t", "t");

        os << "const unsigned int id = " << m_KernelBlockSizes[KernelPresynapticUpdate] << " * blockIdx.x + threadIdx.x; " << std::endl;

        // We need shLg if any synapse groups accumulate into shared memory
        if(std::any_of(model.getLocalSynapseGroups().cbegin(), model.getLocalSynapseGroups().cend(),
            [this](const NNmodel::SynapseGroupValueType &s){ return this->shouldAccumulateInSharedMemory(s.second); }))
        {
            os << "__shared__ " << model.getPrecision() << " shLg[" << m_KernelBlockSizes[KernelPresynapticUpdate] << "];" << std::endl;
        }
        
        // If any of these synapse groups also have ragged connectivity, allocate shared memory for row length
        if(std::any_of(model.getLocalSynapseGroups().cbegin(), model.getLocalSynapseGroups().cend(),
            [&model](const NNmodel::SynapseGroupValueType &s)
            {
                return (s.second.getSpanType() == SynapseGroup::SpanType::POSTSYNAPTIC
                        && (s.second.getMatrixType() & SynapseMatrixConnectivity::RAGGED));
            }))
        {
            os << "__shared__ unsigned int shRowLength[" << m_KernelBlockSizes[KernelPresynapticUpdate] << "];" << std::endl;
        }

        if(std::any_of(model.getLocalSynapseGroups().cbegin(), model.getLocalSynapseGroups().cend(),
            [&model](const NNmodel::SynapseGroupValueType &s)
            { 
                return (s.second.isTrueSpikeRequired() || !s.second.getWUModel()->getLearnPostCode().empty());
            }))
        {
            os << "__shared__ unsigned int shSpk[" << m_KernelBlockSizes[KernelPresynapticUpdate] << "];" << std::endl;
        }
        
        if(std::any_of(model.getLocalSynapseGroups().cbegin(), model.getLocalSynapseGroups().cend(),
            [](const NNmodel::SynapseGroupValueType &s){ return (s.second.isSpikeEventRequired()); }))
        {
            os << "__shared__ unsigned int shSpkEvnt[" << m_KernelBlockSizes[KernelPresynapticUpdate] << "];" << std::endl;
        }
        
        // Parallelise over synapse groups
        genParallelGroup<SynapseGroup>(os, kernelSubs, model.getLocalSynapseGroups(), idPresynapticStart,
            [this](const SynapseGroup &sg){ return padSize(getNumPresynapticUpdateThreads(sg), m_KernelBlockSizes[KernelPresynapticUpdate]); },
            [wumThreshHandler, wumSimHandler, &model, this](CodeStream &os, const SynapseGroup &sg, const Substitutions &popSubs)
            {
                if (sg.getSrcNeuronGroup()->isDelayRequired()) {
                    os << "const unsigned int delaySlot = (dd_spkQuePtr" <<sg.getSrcNeuronGroup()->getName();
                    os << " + " << (sg.getSrcNeuronGroup()->getNumDelaySlots() - sg.getDelaySteps());
                    os << ") % " << sg.getSrcNeuronGroup()->getNumDelaySlots() << ";" << std::endl;
                }

                // If we are going to accumulate postsynaptic input into a register, copy current value into register from global memory
                if (shouldAccumulateInLinSyn(sg)) {
                    os << "// only do this for existing neurons" << std::endl;
                    os << model.getPrecision() << " linSyn;" << std::endl;
                    os << "if(" << popSubs.getVarSubstitution("id") << " < " << sg.getTrgNeuronGroup()->getNumNeurons() << ")";
                    {
                        CodeStream::Scope b(os);
                        os << "linSyn = dd_inSyn" << sg.getName() << "[" << popSubs.getVarSubstitution("id") << "];" << std::endl;
                    }
                }
                // Otherwise, if we are going to accumulate into shared memory, copy current value into correct array index
                // **NOTE** is ok as number of target neurons <= synapseBlkSz
                else if(shouldAccumulateInSharedMemory(sg)) {
                    os << "if(threadIdx.x < " << sg.getTrgNeuronGroup()->getNumNeurons() << ")";
                    {
                        CodeStream::Scope b(os);
                        os << "shLg[threadIdx.x] = dd_inSyn" << sg.getName() << "[threadIdx.x];"<< std::endl;
                    }
                    os << "__syncthreads();" << std::endl;
                }

                // If spike events should be processed
                if (sg.isSpikeEventRequired()) {
                    if(sg.getSpanType() == SynapseGroup::SpanType::PRESYNAPTIC) {
                        assert(sg.getMatrixType() & SynapseMatrixConnectivity::SPARSE);
                        genPresynapticUpdatePreSpan(os, model, sg, popSubs, false,
                                                    wumThreshHandler, wumSimHandler);
                    }
                    else {
                        genPresynapticUpdatePostSpan(os, model, sg, popSubs, false,
                                                     wumThreshHandler, wumSimHandler);
                    }
                }

                // If true spikes should be processed
                if (sg.isTrueSpikeRequired()) {
                    if(sg.getSpanType() == SynapseGroup::SpanType::PRESYNAPTIC) {
                        assert(sg.getMatrixType() & SynapseMatrixConnectivity::SPARSE);
                        genPresynapticUpdatePreSpan(os, model, sg, popSubs, true,
                                                    wumThreshHandler, wumSimHandler);
                    }
                    else {
                        genPresynapticUpdatePostSpan(os, model, sg, popSubs, true,
                                                     wumThreshHandler, wumSimHandler);
                    }
                }
                
                os << std::endl;

                // If we have been accumulating into a register, write value back to global memory
                if (shouldAccumulateInLinSyn(sg)) {
                    os << "// only do this for existing neurons" << std::endl;
                    os << "if (" << popSubs.getVarSubstitution("id") << " < " << sg.getTrgNeuronGroup()->getNumNeurons() << ")";
                    {
                        CodeStream::Scope b(os);
                        os << "dd_inSyn" << sg.getName() << "[" << popSubs.getVarSubstitution("id") << "] = linSyn;" << std::endl;
                    }
                }
                // Otherwise, if we have been accumulating into shared memory, write value back to global memory
                // **NOTE** is ok as number of target neurons <= synapseBlkSz
                else if(shouldAccumulateInSharedMemory(sg)) {
                    os << "__syncthreads();" << std::endl;
                    os << "if (threadIdx.x < " << sg.getTrgNeuronGroup()->getNumNeurons() << ")";
                    {
                        CodeStream::Scope b(os);
                        os << "dd_inSyn" << sg.getName() << "[threadIdx.x] = shLg[threadIdx.x];"<< std::endl;
                    }
                }
            }
        );
    }

    // If any synapse groups require postsynaptic learning
    size_t idPostsynapticStart = 0;
    if(std::any_of(model.getLocalSynapseGroups().cbegin(), model.getLocalSynapseGroups().cend(),
        [](const NNmodel::SynapseGroupValueType &s){ return !s.second.getWUModel()->getLearnPostCode().empty(); }))
    {
        os << "extern \"C\" __global__ void " << KernelNames[KernelPostsynapticUpdate] << "(";
        for (const auto &p : model.getSimLearnPostKernelParameters()) {
            os << p.second << " " << p.first << ", ";
        }
        os << model.getTimePrecision() << " t)" << std::endl; // end of synapse kernel header
        {
            CodeStream::Scope b(os);

            Substitutions kernelSubs(cudaFunctions);
            kernelSubs.addVarSubstitution("t", "t");

            os << "const unsigned int id = " << m_KernelBlockSizes[KernelPostsynapticUpdate] << " * blockIdx.x + threadIdx.x; " << std::endl;
            os << "__shared__ unsigned int shSpk[" << m_KernelBlockSizes[KernelPostsynapticUpdate] << "];" << std::endl;
            if(std::any_of(model.getLocalSynapseGroups().cbegin(), model.getLocalSynapseGroups().cend(),
                [&model](const NNmodel::SynapseGroupValueType &s)
                {
                    return ((s.second.getMatrixType() & SynapseMatrixConnectivity::RAGGED) && !s.second.getWUModel()->getLearnPostCode().empty());
                }))
            {
                os << "__shared__ unsigned int shColLength[" << m_KernelBlockSizes[KernelPostsynapticUpdate] << "];" << std::endl;
            }

            // Parallelise over synapse groups whose weight update models have code for postsynaptic learning
            genParallelGroup<SynapseGroup>(os, kernelSubs, model.getLocalSynapseGroups(), idPostsynapticStart,
                [this](const SynapseGroup &sg){ return padSize(getNumPostsynapticUpdateThreads(sg), m_KernelBlockSizes[KernelPostsynapticUpdate]); },
                [](const SynapseGroup &sg){ return !sg.getWUModel()->getLearnPostCode().empty(); },
                [postLearnHandler, &model, this](CodeStream &os, const SynapseGroup &sg, const Substitutions &popSubs)
                {
                    // If presynaptic neuron group has variable queues, calculate offset to read from its variables with axonal delay
                    if(sg.getSrcNeuronGroup()->isDelayRequired()) {
                        os << "const unsigned int preReadDelayOffset = " << sg.getPresynapticAxonalDelaySlot("dd_") << " * " << sg.getSrcNeuronGroup()->getNumNeurons() << ";" << std::endl;
                    }

                    // If postsynaptic neuron group has variable queues, calculate offset to read from its variables at current time
                    if(sg.getTrgNeuronGroup()->isDelayRequired()) {
                        os << "const unsigned int postReadDelaySlot = " << sg.getPostsynapticBackPropDelaySlot("dd_") << ";" << std::endl;
                        os << "const unsigned int postReadDelayOffset = postReadDelaySlot * " << sg.getTrgNeuronGroup()->getNumNeurons() << ";" << std::endl;
                    }

                    if (sg.getTrgNeuronGroup()->isDelayRequired() && sg.getTrgNeuronGroup()->isTrueSpikeRequired()) {
                        os << "const unsigned int numSpikes = dd_glbSpkCnt" << sg.getTrgNeuronGroup()->getName() << "[postReadDelaySlot];" << std::endl;
                    }
                    else {
                        os << "const unsigned int numSpikes = dd_glbSpkCnt" << sg.getTrgNeuronGroup()->getName() << "[0];" << std::endl;
                    }

                    os << "const unsigned int numSpikeBlocks = (numSpikes + " << m_KernelBlockSizes[KernelPostsynapticUpdate]-1 << ") / " << m_KernelBlockSizes[KernelPostsynapticUpdate] << ";" << std::endl;
                    os << "for (unsigned int r = 0; r < numSpikeBlocks; r++)";
                    {
                        CodeStream::Scope b(os);
                        os << "const unsigned int numSpikesInBlock = (r == numSpikeBlocks - 1) ? ((numSpikes - 1) % " << m_KernelBlockSizes[KernelPostsynapticUpdate] << ") + 1 : " << m_KernelBlockSizes[KernelPostsynapticUpdate] << ";" << std::endl;

                        os << "if (threadIdx.x < numSpikesInBlock)";
                        {
                            CodeStream::Scope b(os);
                            const std::string offsetTrueSpkPost = (sg.getTrgNeuronGroup()->isTrueSpikeRequired() && sg.getTrgNeuronGroup()->isDelayRequired()) ? "postReadDelayOffset + " : "";
                            os << "const unsigned int spk = dd_glbSpk" << sg.getTrgNeuronGroup()->getName() << "[" << offsetTrueSpkPost << "(r * " << m_KernelBlockSizes[KernelPostsynapticUpdate] << ") + threadIdx.x];" << std::endl;
                            os << "shSpk[threadIdx.x] = spk;" << std::endl;

                            if(sg.getMatrixType() & SynapseMatrixConnectivity::RAGGED) {
                                os << "shColLength[threadIdx.x] = dd_colLength" << sg.getName() << "[spk];" << std::endl;
                            }
                        }

                        os << "__syncthreads();" << std::endl;
                        os << "// only work on existing neurons" << std::endl;
                        os << "if (" << popSubs.getVarSubstitution("id") << " < " << sg.getMaxSourceConnections() << ")";
                        {
                            CodeStream::Scope b(os);
                            os << "// loop through all incoming spikes for learning" << std::endl;
                            os << "for (unsigned int j = 0; j < numSpikesInBlock; j++)";
                            {
                                CodeStream::Scope b(os);
                                if (sg.getMatrixType() & SynapseMatrixConnectivity::RAGGED) {
                                    os << "unsigned int synAddress = shSpk[j] * " << std::to_string(sg.getMaxSourceConnections()) << ";" << std::endl;
                                    os << "const unsigned int npre = shColLength[j];" << std::endl;

                                    os << "if (" << popSubs.getVarSubstitution("id") << " < npre)" << CodeStream::OB(1540);
                                    os << "synAddress += " << popSubs.getVarSubstitution("id") << ";" << std::endl;
                                    os << "const unsigned int ipre = dd_remap" + sg.getName() + "[synAddress] / " + std::to_string(sg.getMaxConnections()) + ";" << std::endl;
                                }
                                else {
                                    os << "const unsigned int synAddress = (shSpk[j] * " << std::to_string(sg.getTrgNeuronGroup()->getNumNeurons()) << ") + " << popSubs.getVarSubstitution("id") << ";" << std::endl;
                                }

                                Substitutions synSubs(&popSubs);
                                synSubs.addVarSubstitution("id_pre", "ipre");
                                synSubs.addVarSubstitution("id_post", "shSpk[j]");
                                synSubs.addVarSubstitution("id_syn", "synAddress");

                                postLearnHandler(os, sg, synSubs);

                                if (sg.getMatrixType() & SynapseMatrixConnectivity::RAGGED) {
                                    os << CodeStream::CB(1540);
                                }
                            }
                        }
                    }
                }
            );
        }
    }

    size_t idSynapseDynamicsStart = 0;
    if(std::any_of(model.getLocalSynapseGroups().cbegin(), model.getLocalSynapseGroups().cend(),
        [](const NNmodel::SynapseGroupValueType &s){ return !s.second.getWUModel()->getSynapseDynamicsCode().empty(); }))
    {
        os << "extern \"C\" __global__ void " << KernelNames[KernelSynapseDynamicsUpdate] << "(";
        for (const auto &p : model.getSynapseDynamicsKernelParameters()) {
            os << p.second << " " << p.first << ", ";
        }
        os << model.getTimePrecision() << " t)" << std::endl; // end of synapse kernel header
        {
            CodeStream::Scope b(os);

            Substitutions kernelSubs(cudaFunctions);
            kernelSubs.addVarSubstitution("t", "t");

            // Parallelise over synapse groups whose weight update models have code for synapse dynamics
            genParallelGroup<SynapseGroup>(os, kernelSubs, model.getLocalSynapseGroups(), idSynapseDynamicsStart,
                [this](const SynapseGroup &sg){ return padSize(getNumSynapseDynamicsThreads(sg), m_KernelBlockSizes[KernelSynapseDynamicsUpdate]); },
                [](const SynapseGroup &sg){ return !sg.getWUModel()->getSynapseDynamicsCode().empty(); },
                [synapseDynamicsHandler, &model, this](CodeStream &os, const SynapseGroup &sg, const Substitutions &popSubs)
                {
                    // If presynaptic neuron group has variable queues, calculate offset to read from its variables with axonal delay
                    if(sg.getSrcNeuronGroup()->isDelayRequired()) {
                        os << "const unsigned int preReadDelayOffset = " << sg.getPresynapticAxonalDelaySlot("dd_") << " * " << sg.getSrcNeuronGroup()->getNumNeurons() << ";" << std::endl;
                    }

                    // If postsynaptic neuron group has variable queues, calculate offset to read from its variables at current time
                    if(sg.getTrgNeuronGroup()->isDelayRequired()) {
                        os << "const unsigned int postReadDelayOffset = " << sg.getPostsynapticBackPropDelaySlot("dd_") << " * " << sg.getTrgNeuronGroup()->getNumNeurons() << ";" << std::endl;
                    }

                    Substitutions synSubs(&popSubs);

                    if (sg.getMatrixType() & SynapseMatrixConnectivity::RAGGED) {
                        os << "if (" << popSubs.getVarSubstitution("id") << " < dd_synRemap" << sg.getName() << "[0])";
                    }
                    else {
                        os << "if (" << popSubs.getVarSubstitution("id") << " < " << sg.getSrcNeuronGroup()->getNumNeurons() * sg.getTrgNeuronGroup()->getNumNeurons() << ")";
                    }
                    {
                        CodeStream::Scope b(os);

                        if (sg.getMatrixType() & SynapseMatrixConnectivity::RAGGED) {
                            // Determine synapse and presynaptic indices for this thread
                            os << "const unsigned int s = dd_synRemap" << sg.getName() << "[1 + " << popSubs.getVarSubstitution("id") << "];" << std::endl;

                            synSubs.addVarSubstitution("id_pre", "s / " + std::to_string(sg.getMaxConnections()));
                            synSubs.addVarSubstitution("id_post", "dd_ind" + sg.getName() + "[s]");
                            synSubs.addVarSubstitution("id_syn", "s");
                        }
                        else {
                            synSubs.addVarSubstitution("id_pre", popSubs.getVarSubstitution("id") + " / " + std::to_string(sg.getTrgNeuronGroup()->getNumNeurons()));
                            synSubs.addVarSubstitution("id_post", popSubs.getVarSubstitution("id") + " % " + std::to_string(sg.getTrgNeuronGroup()->getNumNeurons()));
                            synSubs.addVarSubstitution("id_syn", popSubs.getVarSubstitution("id"));

                        }

                        // If dendritic delay is required, always use atomic operation to update dendritic delay buffer
                        if(sg.isDendriticDelayRequired()) {
                            synSubs.addFuncSubstitution("addToInSynDelay", 2, getFloatAtomicAdd(model.getPrecision()) + "(&dd_denDelay" + sg.getPSModelTargetName() + "[" + sg.getDendriticDelayOffset("dd_", "$(1)") + synSubs.getVarSubstitution("id_post") + "], $(0))");
                        }
                        // Otherwise
                        else {
                            synSubs.addFuncSubstitution("addToInSyn", 1, getFloatAtomicAdd(model.getPrecision()) + "(&dd_inSyn" + sg.getPSModelTargetName() + "[" + synSubs.getVarSubstitution("id_post") + "], $(0))");
                        }

                        synapseDynamicsHandler(os, sg, synSubs);
                    }
                });
        }
    }

    os << "void updateSynapses(" << model.getTimePrecision() << " t)";
    {
        CodeStream::Scope b(os);

        // Launch pre-synapse reset kernel if required
        if(idPreSynapseReset > 0) {
            CodeStream::Scope b(os);
            genKernelDimensions(os, KernelPreSynapseReset, idPreSynapseReset);
            os << KernelNames[KernelPreSynapseReset] << "<<<grid, threads>>>();" << std::endl;
        }

        // Launch synapse dynamics kernel if required
        if(idSynapseDynamicsStart > 0) {
            CodeStream::Scope b(os);
            genKernelDimensions(os, KernelSynapseDynamicsUpdate, idPreSynapseReset);
            os << KernelNames[KernelSynapseDynamicsUpdate] << "<<<grid, threads>>>(";
            for(const auto &p : model.getSynapseDynamicsKernelParameters()) {
                os << p.first << ", ";
            }
            os << "t);" << std::endl;
        }

        // Launch presynaptic update kernel
        if(idPresynapticStart > 0) {
            CodeStream::Scope b(os);
            genKernelDimensions(os, KernelPresynapticUpdate, idPresynapticStart);
            os << KernelNames[KernelPresynapticUpdate] << "<<<grid, threads>>>(";
            for(const auto &p : model.getSynapseKernelParameters()) {
                os << p.first << ", ";
            }
            os << "t);" << std::endl;
        }

        // Launch postsynaptic update kernel
        if(idPostsynapticStart > 0) {
            CodeStream::Scope b(os);
            genKernelDimensions(os, KernelPostsynapticUpdate, idPostsynapticStart);
            os << KernelNames[KernelPostsynapticUpdate] << "<<<grid, threads>>>(";
            for(const auto &p : model.getSimLearnPostKernelParameters()) {
                os << p.first << ", ";
            }
            os << "t);" << std::endl;
        }
    }
}
//--------------------------------------------------------------------------
void CUDA::genInit(CodeStream &os, const NNmodel &model,
                   NeuronGroupHandler localNGHandler, NeuronGroupHandler remoteNGHandler,
                   SynapseGroupHandler sgDenseInitHandler, SynapseGroupHandler sgSparseConnectHandler, 
                   SynapseGroupHandler sgSparseInitHandler) const
{
    os << "#include <iostream>" << std::endl;
    os << "#include <random>" << std::endl;
    os << std::endl;

    // If device RNG is required, generate kernel to initialise it
    if(isGlobalRNGRequired(model)) {
        os << "extern \"C\" __global__ void initializeRNGKernel(unsigned long long deviceRNGSeed)";
        {
            CodeStream::Scope b(os);
            os << "if(threadIdx.x == 0)";
            {
                CodeStream::Scope b(os);
                os << "curand_init(deviceRNGSeed, 0, 0, &dd_rng[0]);" << std::endl;
            }
        }
        os << std::endl;
    }

    // init kernel header
    os << "extern \"C\" __global__ void " << KernelNames[KernelInitialize] << "(";
    for(const auto &p : model.getInitKernelParameters()) {
        os << p.second << " " << p.first << ", ";
    }
    os << "unsigned long long deviceRNGSeed)";

    // initialization kernel code
    size_t idInitStart = 0;
    {
        Substitutions kernelSubs(cudaFunctions);

        // common variables for all cases
        CodeStream::Scope b(os);

        os << "const unsigned int id = " << m_KernelBlockSizes[KernelInitialize] << " * blockIdx.x + threadIdx.x;" << std::endl;

        os << "// ------------------------------------------------------------------------" << std::endl;
        os << "// Remote neuron groups" << std::endl;
        genParallelGroup<NeuronGroup>(os, kernelSubs, model.getRemoteNeuronGroups(), idInitStart,
            [this](const NeuronGroup &ng){ return padSize(ng.getNumNeurons(), m_KernelBlockSizes[KernelInitialize]); },
            [this](const NeuronGroup &ng){ return ng.hasOutputToHost(m_LocalHostID); },
            [this, remoteNGHandler](CodeStream &os, const NeuronGroup &ng, Substitutions &popSubs)
            {
                os << "// only do this for existing neurons" << std::endl;
                os << "if(" << popSubs.getVarSubstitution("id") << " < " << ng.getNumNeurons() << ")";
                {
                    CodeStream::Scope b(os);

                    remoteNGHandler(os, ng, popSubs);
                }
            });
        os << std::endl;
   
        os << "// ------------------------------------------------------------------------" << std::endl;
        os << "// Local neuron groups" << std::endl;
        genParallelGroup<NeuronGroup>(os, kernelSubs, model.getLocalNeuronGroups(), idInitStart,
            [this](const NeuronGroup &ng){ return padSize(ng.getNumNeurons(), m_KernelBlockSizes[KernelInitialize]); },
            [this](const NeuronGroup &ng){ return ng.isInitCodeRequired(); },
            [this, &model, localNGHandler](CodeStream &os, const NeuronGroup &ng, Substitutions &popSubs)
            {
                os << "// only do this for existing neurons" << std::endl;
                os << "if(" << popSubs.getVarSubstitution("id") << " < " << ng.getNumNeurons() << ")";
                {
                    CodeStream::Scope b(os);
                    // If this neuron is going to require a simulation RNG, initialise one using GLOBAL thread id for sequence
                    if(ng.isSimRNGRequired()) {
                        os << "curand_init(deviceRNGSeed, id, 0, &dd_rng" << ng.getName() << "[" << popSubs.getVarSubstitution("id") << "]);" << std::endl;
                    }

                    // If this neuron requires an RNG for initialisation,
                    // make copy of global phillox RNG and skip ahead by thread id
                    // **NOTE** not LOCAL id
                    if(ng.isInitRNGRequired()) {
                        os << "curandStatePhilox4_32_10_t initRNG = dd_rng[0];" << std::endl;
                        os << "skipahead_sequence((unsigned long long)id, &initRNG);" << std::endl;

                        // Add substitution for RNG
                        popSubs.addVarSubstitution("rng", "&initRNG");
                    }

                    localNGHandler(os, ng, popSubs);
                }
            });
        os << std::endl;

        os << "// ------------------------------------------------------------------------" << std::endl;
        os << "// Synapse groups with dense connectivity" << std::endl;
        genParallelGroup<SynapseGroup>(os, kernelSubs, model.getLocalSynapseGroups(), idInitStart,
            [this](const SynapseGroup &sg){ return padSize(sg.getTrgNeuronGroup()->getNumNeurons(), m_KernelBlockSizes[KernelInitialize]); },
            [](const SynapseGroup &sg){ return (sg.getMatrixType() & SynapseMatrixConnectivity::DENSE) && (sg.getMatrixType() & SynapseMatrixWeight::INDIVIDUAL) && sg.isWUVarInitRequired(); },
            [sgDenseInitHandler](CodeStream &os, const SynapseGroup &sg, Substitutions &popSubs)
            {
                os << "// only do this for existing postsynaptic neurons" << std::endl;
                os << "if(" << popSubs.getVarSubstitution("id") << " < " << sg.getTrgNeuronGroup()->getNumNeurons() << ")";
                {
                    CodeStream::Scope b(os);
                    // If this post synapse requires an RNG for initialisation,
                    // make copy of global phillox RNG and skip ahead by thread id
                    // **NOTE** not LOCAL id
                    if(sg.isWUInitRNGRequired()) {
                        os << "curandStatePhilox4_32_10_t initRNG = dd_rng[0];" << std::endl;
                        os << "skipahead_sequence((unsigned long long)id, &initRNG);" << std::endl;

                        // Add substitution for RNG
                        popSubs.addVarSubstitution("rng", "&initRNG");
                    }

                    popSubs.addVarSubstitution("id_post", popSubs.getVarSubstitution("id"));
                    sgDenseInitHandler(os, sg, popSubs);
                }
            });
        os << std::endl;

        os << "// ------------------------------------------------------------------------" << std::endl;
        os << "// Synapse groups with sparse connectivity" << std::endl;
        genParallelGroup<SynapseGroup>(os, kernelSubs, model.getLocalSynapseGroups(), idInitStart,
            [this](const SynapseGroup &sg){ return padSize(sg.getSrcNeuronGroup()->getNumNeurons(), m_KernelBlockSizes[KernelInitialize]); },
            [](const SynapseGroup &sg){ return sg.isSparseConnectivityInitRequired(); },
            [sgSparseConnectHandler](CodeStream &os, const SynapseGroup &sg, Substitutions &popSubs)
            {
                const size_t numSrcNeurons = sg.getSrcNeuronGroup()->getNumNeurons();
                const size_t numTrgNeurons = sg.getTrgNeuronGroup()->getNumNeurons();

                os << "// only do this for existing presynaptic neurons" << std::endl;
                os << "if(" << popSubs.getVarSubstitution("id") << " < " << numSrcNeurons << ")";
                {
                    CodeStream::Scope b(os);
                    // If this connectivity requires an RNG for initialisation,
                    // make copy of global phillox RNG and skip ahead by thread id
                    // **NOTE** not LOCAL id
                    if(::isRNGRequired(sg.getConnectivityInitialiser().getSnippet()->getRowBuildCode())) {
                        os << "curandStatePhilox4_32_10_t initRNG = dd_rng[0];" << std::endl;
                        os << "skipahead_sequence((unsigned long long)id, &initRNG);" << std::endl;

                        // Add substitution for RNG
                        popSubs.addVarSubstitution("rng", "&initRNG");
                    }

                    // If the synapse group has bitmask connectivity
                    if(sg.getMatrixType() & SynapseMatrixConnectivity::BITMASK) {
                        // Calculate indices of bits at start and end of row
                        os << "// Calculate indices" << std::endl;
                        const size_t maxSynapses = numSrcNeurons * numTrgNeurons;
                        if((maxSynapses & 0xFFFFFFFF00000000ULL) != 0) {
                            os << "const uint64_t rowStartGID = " << popSubs.getVarSubstitution("id") << " * " << numTrgNeurons << "ull;" << std::endl;
                        }
                        else {
                            os << "const unsigned int rowStartGID = " << popSubs.getVarSubstitution("id") << " * " << numTrgNeurons << ";" << std::endl;
                        }

                        // Build function template to set correct bit in bitmask
                        popSubs.addFuncSubstitution("addSynapse", 1,
                                                    "atomicOr(&dd_gp" + sg.getName() + "[(rowStartGID + $(0)) / 32], 0x80000000 >> ((rowStartGID + $(0)) & 31))");
                    }
                    // Otherwise, if synapse group has ragged connectivity
                    else if(sg.getMatrixType() & SynapseMatrixConnectivity::RAGGED) {
                        const std::string rowLength = "dd_rowLength" + sg.getName() + "[" + popSubs.getVarSubstitution("id") + "]";
                        const std::string ind = "dd_ind" + sg.getName();

                        // Zero row length
                        os << rowLength << " = 0;" << std::endl;

                        // Build function template to increment row length and insert synapse into ind array
                        popSubs.addFuncSubstitution("addSynapse", 1,
                                                    ind + "[(" + popSubs.getVarSubstitution("id") + " * " + std::to_string(sg.getMaxConnections()) + ") + (" + rowLength + "++)] = $(0)");
                    }
                    else {
                        assert(false);
                    }

                    popSubs.addVarSubstitution("id_pre", popSubs.getVarSubstitution("id"));
                    sgSparseConnectHandler(os, sg, popSubs);
                }
            });
    }
    os << std::endl;
    const unsigned int numStaticInitThreads = idInitStart;

    // Sparse initialization kernel code
    size_t idSparseInitStart = 0;
    if(std::any_of(model.getLocalSynapseGroups().cbegin(), model.getLocalSynapseGroups().cend(),
        [](const NNmodel::SynapseGroupValueType &s) { return s.second.isSparseInitRequired(); })) 
    {
        os << "extern \"C\" __global__ void " << KernelNames[KernelInitializeSparse] << "()";
        {
            CodeStream::Scope b(os);

            // common variables for all cases
            Substitutions kernelSubs(cudaFunctions);

            os << "const unsigned int id = " << m_KernelBlockSizes[KernelInitializeSparse] << " * blockIdx.x + threadIdx.x;" << std::endl;

            // Shared memory array so row lengths don't have to be read by EVERY postsynaptic thread
            // **TODO** check actually required
            os << "__shared__ unsigned int shRowLength[" << m_KernelBlockSizes[KernelInitializeSparse] << "];" << std::endl;
            os << "__shared__ unsigned int shRowStart[" << m_KernelBlockSizes[KernelInitializeSparse] + 1 << "];" << std::endl;

            // Initialise weight update variables for synapse groups with dense connectivity
            genParallelGroup<SynapseGroup>(os, kernelSubs, model.getLocalSynapseGroups(), idSparseInitStart,
                [this](const SynapseGroup &sg){ return padSize(sg.getMaxConnections(), m_KernelBlockSizes[KernelInitializeSparse]); },
                [](const SynapseGroup &sg){ return sg.isSparseInitRequired(); },
                [this, &model, sgSparseInitHandler, numStaticInitThreads](CodeStream &os, const SynapseGroup &sg, Substitutions &popSubs)
                {
                    // If this post synapse requires an RNG for initialisation,
                    // make copy of global phillox RNG and skip ahead by thread id
                    // **NOTE** not LOCAL id
                    if(sg.isWUInitRNGRequired()) {
                        os << "curandStatePhilox4_32_10_t initRNG = dd_rng[0];" << std::endl;
                        os << "skipahead_sequence((unsigned long long)" << numStaticInitThreads << " + id, &initRNG);" << std::endl;

                        // Add substitution for RNG
                        popSubs.addVarSubstitution("rng", "&initRNG");
                    }

                    os << "unsigned int idx = " << popSubs.getVarSubstitution("id") << ";" << std::endl;

                    // Calculate how many blocks rows need to be processed in (in order to store row lengths in shared memory)
                    const unsigned int numSrcNeurons = sg.getSrcNeuronGroup()->getNumNeurons();
                    const unsigned int numBlocks = ceilDivide(numSrcNeurons, m_KernelBlockSizes[KernelInitializeSparse]);

                    // Loop through blocks
                    os << "for(unsigned int r = 0; r < " << numBlocks << "; r++)";
                    {
                        CodeStream::Scope b(os);

                        // Calculate number of rows to process in this block
                        os << "const unsigned numRowsInBlock = (r == " << numBlocks - 1 << ")";
                        os << " ? " << ((numSrcNeurons - 1) % m_KernelBlockSizes[KernelInitializeSparse]) + 1;
                        os << " : " << m_KernelBlockSizes[KernelInitializeSparse] << ";" << std::endl;

                        // Use threads to copy block of sparse structure into shared memory
                        os << "__syncthreads();" << std::endl;
                        os << "if (threadIdx.x < numRowsInBlock)";
                        {
                            CodeStream::Scope b(os);
                            os << "shRowLength[threadIdx.x] = dd_rowLength" << sg.getName() << "[(r * " << m_KernelBlockSizes[KernelInitializeSparse] << ") + threadIdx.x];" << std::endl;
                        }

                        // If this synapse projection has ragged connectivity initialised on device and has synapse dynamics
                        if(sg.isSparseConnectivityInitRequired()
                            && (sg.getMatrixType() & SynapseMatrixConnectivity::RAGGED)
                            && !sg.getWUModel()->getSynapseDynamicsCode().empty())
                        {
                            // Use first thread to generate cumulative sum
                            os << "if (threadIdx.x == 0)";
                            {
                                CodeStream::Scope b(os);

                                // Get index of last row in resultant synapse dynamics structure
                                // **NOTE** if there IS a previous block, it will always have had initSparseBlkSz rows in it
                                os << "unsigned int rowStart = (r == 0) ? 0 : shRowStart[" << m_KernelBlockSizes[KernelInitializeSparse] << "];" << std::endl;
                                os << "shRowStart[0] = rowStart;" << std::endl;

                                // Loop through rows in block
                                os << "for(unsigned int i = 0; i < numRowsInBlock; i++)";
                                {
                                    CodeStream::Scope b(os);

                                    // Add this row's length to cumulative sum and write this to this row's end
                                    os << "rowStart += shRowLength[i];" << std::endl;
                                    os << "shRowStart[i + 1] = rowStart;" << std::endl;
                                }

                                // If this is the first thread block and the last block of rows,
                                // write the total cumulative sum to the first entry of the remap structure
                                os << "if(blockIdx.x == 0 && (r == " << numBlocks - 1 << "))";
                                {
                                    CodeStream::Scope b(os);
                                    os << "dd_synRemap" << sg.getName() << "[0] = shRowStart[numRowsInBlock];" << std::endl;
                                }

                            }
                        }

                        os << "__syncthreads();" << std::endl;

                        // Loop through rows
                        os << "for(unsigned int i = 0; i < numRowsInBlock; i++)";
                        {
                            CodeStream::Scope b(os);

                            // If there is a synapse for this thread to initialise
                            os << "if(" << popSubs.getVarSubstitution("id") << " < shRowLength[i])";
                            {
                                CodeStream::Scope b(os);

                                popSubs.addVarSubstitution("id_syn", "idx");
                                popSubs.addVarSubstitution("id_pre", "((r * " + std::to_string(m_KernelBlockSizes[KernelInitializeSparse]) + ") + i)");
                                popSubs.addVarSubstitution("id_post", "dd_ind" + sg.getName() + "[idx]");
                                sgSparseInitHandler(os, sg, popSubs);

                                // If matrix is ragged, connectivity is initialised on device and postsynaptic learning is required
                                if((sg.getMatrixType() & SynapseMatrixConnectivity::RAGGED)
                                    && sg.isSparseConnectivityInitRequired())
                                {
                                    // If postsynaptic learning is required
                                    if(!sg.getWUModel()->getSynapseDynamicsCode().empty()) {
                                        CodeStream::Scope b(os);

                                        // Extract index of synapse's postsynaptic target
                                        os << "const unsigned int postIndex = dd_ind" << sg.getName() << "[idx];" << std::endl;

                                        // Atomically increment length of column of connectivity associated with this target
                                        // **NOTE** this returns previous length i.e. where to insert new entry
                                        os << "const unsigned int colLocation = atomicAdd(&dd_colLength" << sg.getName() << "[postIndex], 1);" << std::endl;

                                        // From this calculate index into column-major matrix
                                        os << "const unsigned int colMajorIndex = (postIndex * " << sg.getMaxSourceConnections() << ") + colLocation;" << std::endl;

                                        // Add remapping entry at this location poining back to row-major index
                                        os << "dd_remap" << sg.getName() << "[colMajorIndex] = idx;" << std::endl;
                                    }

                                    // If synapse dynamics are required, copy idx into syn remap structure
                                    if(!sg.getWUModel()->getSynapseDynamicsCode().empty()) {
                                        CodeStream::Scope b(os);
                                        os << "dd_synRemap" << sg.getName() << "[shRowStart[i] + lid + 1] = idx;" << std::endl;
                                    }
                                }
                            }

                            // If matrix is ragged, advance index to next row by adding stride
                            os << "idx += " << sg.getMaxConnections() << ";" << std::endl;
                        }
                    }
                });
        }
        os << std::endl;
    }

    os << "void initialize()";
    {
        CodeStream::Scope b(os);

        // Generate test for GLIBC test
        genGLIBCBugTest(os);

        os << "unsigned long long deviceRNGSeed = 0;" << std::endl;

        // If on-device global RNG is required
        if(isGlobalRNGRequired(model)) {
            // If no seed is specified
            if (model.getSeed() == 0) {
                CodeStream::Scope b(os);

                // Use system randomness to generate one unsigned long long worth of seed words
                os << "std::random_device seedSource;" << std::endl;
                os << "uint32_t *deviceRNGSeedWord = reinterpret_cast<uint32_t*>(&deviceRNGSeed);" << std::endl;
                os << "for(int i = 0; i < " << sizeof(unsigned long long) / sizeof(uint32_t) << "; i++)";
                {
                    CodeStream::Scope b(os);
                    os << "deviceRNGSeedWord[i] = seedSource();" << std::endl;
                }
            }
            // Otherwise, use model seed
            else {
                os << "deviceRNGSeed = " << model.getSeed() << ";" << std::endl;
            }

            // Launch kernel to initalize RNG
            os << "initializeRNGKernel<<<1, 1>>>(deviceRNGSeed);" << std::endl;
        }

        for(const auto &s : model.getLocalSynapseGroups()) {
            if(s.second.isSparseConnectivityInitRequired()) {
                // If this synapse population has BITMASK connectivity, insert a call to cudaMemset to zero the whole bitmask
                if(s.second.getMatrixType() & SynapseMatrixConnectivity::BITMASK) {
                    const size_t gpSize = ((size_t)s.second.getSrcNeuronGroup()->getNumNeurons() * (size_t)s.second.getTrgNeuronGroup()->getNumNeurons()) / 32 + 1;
                    os << "cudaMemset(d_gp" << s.first << ", 0, " << gpSize << " * sizeof(uint32_t));" << std::endl;
                }
                // If this synapse population has RAGGED connectivity and has postsynaptic learning, insert a call to cudaMemset to zero column lengths
                else if((s.second.getMatrixType() & SynapseMatrixConnectivity::RAGGED)
                    && !s.second.getWUModel()->getLearnPostCode().empty())
                {
                    os << "cudaMemset(d_colLength" << s.first << ", 0, " << s.second.getTrgNeuronGroup()->getNumNeurons() << " * sizeof(unsigned int));" << std::endl;
                }
            }
        }
//
        // If there are any initialisation threads
        if(idInitStart > 0) {
            genKernelDimensions(os, KernelInitialize, idInitStart);
            os << KernelNames[KernelInitialize] << "<<<grid, threads>>>(";
            for(const auto &p : model.getInitKernelParameters()) {
                os << p.first << ", ";
            }
            os << "deviceRNGSeed);" << std::endl;
        }
    }
    os << std::endl;
    os << "void initializeSparse()";
    {
        CodeStream::Scope b(os);

        // Copy all uninitialised state variables to device
        os << "copyStateToDevice(true);" << std::endl << std::endl;

        // If there are any sparse initialisation threads
        if(idSparseInitStart > 0) {
            genKernelDimensions(os, KernelInitializeSparse, idSparseInitStart);
            os << KernelNames[KernelInitializeSparse] << "<<<grid, threads>>>();" << std::endl;
        }
    }
}
//--------------------------------------------------------------------------
void CUDA::genDefinitionsPreamble(CodeStream &os) const
{
    os << "// Standard C++ includes" << std::endl;
    os << "#include <string>" << std::endl;
    os << "#include <stdexcept>" << std::endl;
    os << std::endl;
    os << "// CUDA includes" << std::endl;
    os << "#include <curand_kernel.h>" << std::endl;
    os << std::endl;
    os << "// ------------------------------------------------------------------------" << std::endl;
    os << "// Helper macro for error-checking CUDA calls" << std::endl;
    os << "#define CHECK_CUDA_ERRORS(call) {\\" << std::endl;
    os << "    cudaError_t error = call;\\" << std::endl;
    os << "    if (error != cudaSuccess) {\\" << std::endl;
    os << "        throw std::runtime_error(__FILE__\": \" + std::to_string(__LINE__) + \": cuda error \" + std::to_string(error) + \": \" + cudaGetErrorString(error));\\" << std::endl;
    os << "    }\\" << std::endl;
    os << "}" << std::endl;
}
//--------------------------------------------------------------------------
void CUDA::genRunnerPreamble(CodeStream &os) const
{
    // **TODO** move these into a header file shipped with GeNN and copied into generated code along with non-uniform RNGs
    os << "// ------------------------------------------------------------------------" << std::endl;
    os << "// Helper function for allocating memory blocks on the GPU device" << std::endl;
    os << std::endl;
    os << "template<class T>" << std::endl;
    os << "void deviceMemAllocate(T* hostPtr, const T &devSymbol, size_t size)";
    {
        CodeStream::Scope b(os);
        os << "void *devptr;" << std::endl;
        os << "CHECK_CUDA_ERRORS(cudaMalloc(hostPtr, size));" << std::endl;
        os << "CHECK_CUDA_ERRORS(cudaGetSymbolAddress(&devptr, devSymbol));" << std::endl;
        os << "CHECK_CUDA_ERRORS(cudaMemcpy(devptr, hostPtr, sizeof(void*), cudaMemcpyHostToDevice));" << std::endl;
    }
    os << std::endl;

    os << "// ------------------------------------------------------------------------" << std::endl;
    os << "// Helper function for getting the device pointer corresponding to a zero-copied host pointer and assigning it to a symbol" << std::endl;
    os << std::endl;
    os << "template<class T>" << std::endl;
    os << "void deviceZeroCopy(T hostPtr, const T *devPtr, const T &devSymbol)";
    {
        CodeStream::Scope b(os);
        os << "CHECK_CUDA_ERRORS(cudaHostGetDevicePointer((void **)devPtr, (void*)hostPtr, 0));" << std::endl;
        os << "void *devSymbolPtr;" << std::endl;
        os << "CHECK_CUDA_ERRORS(cudaGetSymbolAddress(&devSymbolPtr, devSymbol));" << std::endl;
        os << "CHECK_CUDA_ERRORS(cudaMemcpy(devSymbolPtr, devPtr, sizeof(void*), cudaMemcpyHostToDevice));" << std::endl;
    }
    os << std::endl;
}
//--------------------------------------------------------------------------
void CUDA::genAllocateMemPreamble(CodeStream &os, const NNmodel &model) const
{
    // Get chosen device's PCI bus ID
    char pciBusID[32];
    CHECK_CUDA_ERRORS(cudaDeviceGetPCIBusId(pciBusID, 32, m_ChosenDeviceID));

    // Write code to get device by PCI bus ID
    // **NOTE** this is required because device IDs are not guaranteed to remain the same and we want the code to be run on the same GPU it was optimise for
    os << "int deviceID;" << std::endl;
    os << "CHECK_CUDA_ERRORS(cudaDeviceGetByPCIBusId(&deviceID, \"" << pciBusID << "\"));" << std::endl;
    os << "CHECK_CUDA_ERRORS(cudaSetDevice(deviceID));" << std::endl;

    // If the model requires zero-copy
    if(model.zeroCopyInUse()) {
        // If device doesn't support mapping host memory error
        if(!getChosenCUDADevice().canMapHostMemory) {
            throw std::runtime_error("Device does not support mapping CPU host memory!");
        }

        // set appropriate device flags
        os << "CHECK_CUDA_ERRORS(cudaSetDeviceFlags(cudaDeviceMapHost));" << std::endl;
    }
}
//--------------------------------------------------------------------------
void CUDA::genVariableDefinition(CodeStream &os, const std::string &type, const std::string &name, VarLocation loc) const
{
    if(loc & VarLocation::HOST) {
        os << getVarExportPrefix() << " " << type << " " << name << ";" << std::endl;
    }
    if(loc & VarLocation::DEVICE) {
        os << getVarExportPrefix() << " " << type << " d_" << name << ";" << std::endl;
        os << getVarExportPrefix() << " __device__ " << type << " dd_" << name << ";" << std::endl;
    }
}
//--------------------------------------------------------------------------
void CUDA::genVariableImplementation(CodeStream &os, const std::string &type, const std::string &name, VarLocation loc) const
{
    if(loc & VarLocation::HOST) {
        os << type << " " << name << ";" << std::endl;
    }
    if(loc & VarLocation::DEVICE) {
        os << type << " d_" << name << ";" << std::endl;
        os << "__device__ " << type << " dd_" << name << ";" << std::endl;
    }
}
//--------------------------------------------------------------------------
void CUDA::genVariableAllocation(CodeStream &os, const std::string &type, const std::string &name, VarLocation loc, size_t count) const
{
    if(loc & VarLocation::HOST) {
        // **NOTE** because we want out memory to be pinned for faster copying to GPU, DON'T use host code generator
        const char *flags = (loc & VarLocation::ZERO_COPY) ? "cudaHostAllocMapped" : "cudaHostAllocPortable";
        os << "cudaHostAlloc(&" << name << ", " << count << " * sizeof(" << type << "), " << flags << ");" << std::endl;
    }

    // If variable is present on device at all
    if(loc & VarLocation::DEVICE) {
        // Insert call to correct helper depending on whether variable should be allocated in zero-copy mode or not
        if(loc & VarLocation::ZERO_COPY) {
            os << "deviceZeroCopy(" << name << ", &d_" << name << ", dd_" << name << ");" << std::endl;
        }
        else {
            os << "deviceMemAllocate(&d_" << name << ", dd_" << name << ", " << count << " * sizeof(" << type << "));" << std::endl;
        }
    }
}
//--------------------------------------------------------------------------
void CUDA::genVariableFree(CodeStream &os, const std::string &name, VarLocation loc) const
{
    // **NOTE** because we pinned the variable we need to free it with cudaFreeHost rather than use the host code generator
    if(loc & VarLocation::HOST) {
        os << "CHECK_CUDA_ERRORS(cudaFreeHost(" << name << "));" << std::endl;
    }

    // If this variable wasn't allocated in zero-copy mode, free it
    if(loc & VarLocation::DEVICE) {
        os << "CHECK_CUDA_ERRORS(cudaFree(d_" << name << "));" << std::endl;
    }
}
//--------------------------------------------------------------------------
void CUDA::genPopVariableInit(CodeStream &os, VarLocation, const Substitutions &kernelSubs, Handler handler) const
{
    Substitutions varSubs(&kernelSubs);

    // If this is first thread in group
    os << "if(" << varSubs.getVarSubstitution("id") << " == 0)";
    {
        CodeStream::Scope b(os);
        handler(os, varSubs);
    }
}
//--------------------------------------------------------------------------
void CUDA::genVariableInit(CodeStream &os, VarLocation, size_t, const std::string &countVarName,
                           const Substitutions &kernelSubs, Handler handler) const
{
    // Variable should already be provided via parallelism
    assert(kernelSubs.hasVarSubstitution(countVarName));

    Substitutions varSubs(&kernelSubs);
    handler(os, varSubs);
}
//--------------------------------------------------------------------------
void CUDA::genVariablePush(CodeStream &os, const std::string &type, const std::string &name, VarLocation loc, bool autoInitialized, size_t count) const
{
    // If variable can be pushed or pulled
    if(canPushPullVar(loc)) {
        // If variable is initialised on device, only copy if uninitialisedOnly isn't set
        if(autoInitialized) {
            os << "if(!uninitialisedOnly)" << CodeStream::OB(1101);
        }

        os << "CHECK_CUDA_ERRORS(cudaMemcpy(d_" << name;
        os << ", " << name;
        os << ", " << count << " * sizeof(" << type << "), cudaMemcpyHostToDevice));" << std::endl;

        if(autoInitialized) {
            os << CodeStream::CB(1101);
        }
    }
}
//--------------------------------------------------------------------------
void CUDA::genVariablePull(CodeStream &os, const std::string &type, const std::string &name, VarLocation loc, size_t count) const
{
    // If variable can be pushed or pulled
    if(canPushPullVar(loc)) {
        os << "CHECK_CUDA_ERRORS(cudaMemcpy(" << name;
        os << ", d_"  << name;
        os << ", " << count << " * sizeof(" << type << "), cudaMemcpyDeviceToHost));" << std::endl;
    }
}
//--------------------------------------------------------------------------
void CUDA::genGlobalRNG(CodeStream &definitions, CodeStream &runner, CodeStream &allocations, CodeStream &free, const NNmodel &) const
{
    // Create a single Philox4_32_10 RNG
    genVariableDefinition(definitions, "curandStatePhilox4_32_10_t*", "rng", VarLocation::DEVICE);
    genVariableImplementation(runner, "curandStatePhilox4_32_10_t*", "rng", VarLocation::DEVICE);
    genVariableAllocation(allocations, "curandStatePhilox4_32_10_t", "rng", VarLocation::DEVICE, 1);
    genVariableFree(free, "rng", VarLocation::DEVICE);
}
//--------------------------------------------------------------------------
void CUDA::genPopulationRNG(CodeStream &definitions, CodeStream &runner, CodeStream &allocations, CodeStream &free,
                             const std::string &name, size_t count) const
{
    // Create an array or XORWOW RNGs
    genArray(definitions, runner, allocations, free, "curandState", name, VarLocation::DEVICE, count);
}
//--------------------------------------------------------------------------
void CUDA::genMakefilePreamble(std::ostream &os) const
{
    const std::string architecture = "sm_" + std::to_string(getChosenCUDADevice().major) + std::to_string(getChosenCUDADevice().minor);
    std::string linkFlags = "--shared --linker-options '-fPIC' -arch " + architecture;

    // Write variables to preamble
    os << "NVCC := nvcc" << std::endl;
    os << "NVCCFLAGS := " << getNVCCFlags() << std::endl;
    os << "LINKFLAGS := " << linkFlags << std::endl;
}
//--------------------------------------------------------------------------
void CUDA::genMakefileLinkRule(std::ostream &os) const
{
    os << "\t$(NVCC) $(LINKFLAGS) -o $@ $(OBJECTS)" << std::endl;
}
//--------------------------------------------------------------------------
void CUDA::genMakefileCompileRule(std::ostream &os) const
{
    // Add one rule to generate dependency files from cc files
    os << "%.d: %.cc" << std::endl;
    os << "\t$(NVCC) -M $(NVCCFLAGS) $< 1> $@" << std::endl;
    os << std::endl;

    // Add another to build object files from cc files
    os << "%.o: %.cc %.d" << std::endl;
    os << "\t$(NVCC) -dc $(NVCCFLAGS) $<" << std::endl;
}
//--------------------------------------------------------------------------
bool CUDA::isGlobalRNGRequired(const NNmodel &model) const
{
    // If any neuron groups require  RNG for initialisation, return true
    // **NOTE** this takes postsynaptic model initialisation into account
    if(std::any_of(model.getLocalNeuronGroups().cbegin(), model.getLocalNeuronGroups().cend(),
        [](const NNmodel::NeuronGroupValueType &n)
        {
            return n.second.isInitRNGRequired();
        }))
    {
        return true;
    }

    // If any synapse groups require an RNG for weight update model initialisation, return true
    if(std::any_of(model.getLocalSynapseGroups().cbegin(), model.getLocalSynapseGroups().cend(),
        [](const NNmodel::SynapseGroupValueType &s)
        {
            return s.second.isWUInitRNGRequired();
        }))
    {
        return true;
    }

    return false;
}
//--------------------------------------------------------------------------
std::string CUDA::getNVCCFlags() const
{
    const std::string architecture = "sm_" + std::to_string(getChosenCUDADevice().major) + std::to_string(getChosenCUDADevice().minor);
    std::string nvccFlags = "-std=c++11 --compiler-options '-fPIC' -x cu -arch " + architecture;
    nvccFlags += " " + m_Preferences.userNvccFlags;
    if (m_Preferences.optimizeCode) {
        nvccFlags += " -O3 -use_fast_math -Xcompiler \"-ffast-math\"";
    }
    if (m_Preferences.debugCode) {
        nvccFlags += " -O0 -g -G";
    }
    if (m_Preferences.showPtxInfo) {
        nvccFlags += " -Xptxas \"-v\"";
    }
#ifdef MPI_ENABLE
    // If MPI is enabled, add MPI include path
    nvccFlags +=" -I\"$(MPI_PATH)/include\"";
#endif
    return nvccFlags;
}
//--------------------------------------------------------------------------
size_t CUDA::getNumPresynapticUpdateThreads(const SynapseGroup &sg)
{
     if (sg.getMatrixType() & SynapseMatrixConnectivity::SPARSE) {
        if (sg.getSpanType() == SynapseGroup::SpanType::PRESYNAPTIC) {
            return sg.getSrcNeuronGroup()->getNumNeurons();
        }
        else {
            // paddedSize is the lowest multiple of blockSize >= maxConn[i]
            return sg.getMaxConnections();
        }
    }
    else {
        // paddedSize is the lowest multiple of blockSize >= neuronN[synapseTarget[i]]
        return sg.getTrgNeuronGroup()->getNumNeurons();
    }
}
//--------------------------------------------------------------------------
size_t CUDA::getNumPostsynapticUpdateThreads(const SynapseGroup &sg)
{
    if (sg.getMatrixType() & SynapseMatrixConnectivity::SPARSE) {
        return sg.getMaxSourceConnections();
    }
    else {
        return sg.getSrcNeuronGroup()->getNumNeurons();
    }
}
//--------------------------------------------------------------------------
size_t CUDA::getNumSynapseDynamicsThreads(const SynapseGroup &sg)
{
    if (sg.getMatrixType() & SynapseMatrixConnectivity::SPARSE) {
        return sg.getSrcNeuronGroup()->getNumNeurons() * sg.getMaxConnections();
    }
    else {
        return sg.getSrcNeuronGroup()->getNumNeurons() * sg.getTrgNeuronGroup()->getNumNeurons();
    }
}
//--------------------------------------------------------------------------
CUDA CUDA::create(const NNmodel &model, const filesystem::path &outputPath, int localHostID, 
                  const Preferences &preferences, Generator generator)
{
    if(preferences.autoChooseDevice) {
        KernelBlockSize cudaBlockSize;
        const int deviceID = chooseOptimalDevice(model, cudaBlockSize, generator, outputPath);

        // Create backends
        CUDA backend(cudaBlockSize, localHostID, deviceID);
        return std::move(backend);
    }
    else {
        const int deviceID = chooseDeviceWithMostGlobalMemory();
        
        // Optimise block size
        KernelBlockSize cudaBlockSize;
        optimizeBlockSize(deviceID, model, cudaBlockSize, preferences, generator, outputPath);
        
        // Create backends
        CUDA backend(cudaBlockSize, localHostID, deviceID);
        return std::move(backend);
    }
}
//--------------------------------------------------------------------------
void CUDA::genEmitSpike(CodeStream &os, const Substitutions &subs, const std::string &suffix) const
{
    os << "const unsigned int spk" << suffix << "Idx = atomicAdd((unsigned int *) &shSpk" << suffix << "Count, 1);" << std::endl;
    os << "shSpk" << suffix << "[spk" << suffix << "Idx] = " << subs.getVarSubstitution("id") << ";" << std::endl;
}
//--------------------------------------------------------------------------
void CUDA::genCurrentSpikePush(CodeStream &os, const NeuronGroup &ng, bool spikeEvent) const
{
    // Is push required at all
    const bool pushRequired = spikeEvent ?
        (ng.isSpikeEventRequired() && canPushPullVar(ng.getSpikeEventLocation()))
        : canPushPullVar(ng.getSpikeLocation());

    // Is delay required
    const bool delayRequired = spikeEvent ?
        ng.isDelayRequired() :
        (ng.isTrueSpikeRequired() && ng.isDelayRequired());

    const char *spikeCntPrefix = spikeEvent ? "glbSpkCntEvnt" : "glbSpkCnt";
    const char *spikePrefix = spikeEvent ? "glbSpkEvnt" : "glbSpk";

    if(pushRequired) {
        if (delayRequired) {
            os << "CHECK_CUDA_ERRORS(cudaMemcpy(d_" << spikeCntPrefix << ng.getName() << "+spkQuePtr" << ng.getName();
            os << ", " << spikeCntPrefix << ng.getName() << " + spkQuePtr" << ng.getName();
            os << ", sizeof(unsigned int), cudaMemcpyHostToDevice));" << std::endl;
            os << "CHECK_CUDA_ERRORS(cudaMemcpy(d_" << spikePrefix << ng.getName() << " + (spkQuePtr" << ng.getName() << "*" << ng.getNumNeurons() << ")";
            os << ", " << spikePrefix << ng.getName();
            os << "+(spkQuePtr" << ng.getName() << " * " << ng.getNumNeurons() << ")";
            os << ", " << spikeCntPrefix << ng.getName() << "[spkQuePtr" << ng.getName() << "] * sizeof(unsigned int), cudaMemcpyHostToDevice));" << std::endl;
        }
        else {
            os << "CHECK_CUDA_ERRORS(cudaMemcpy(d_" << spikeCntPrefix << ng.getName();
            os << ", " << spikeCntPrefix << ng.getName();
            os << ", sizeof(unsigned int), cudaMemcpyHostToDevice));" << std::endl;
            os << "CHECK_CUDA_ERRORS(cudaMemcpy(d_" << spikePrefix << ng.getName();
            os << ", " << spikePrefix << ng.getName();
            os << ", " << spikeCntPrefix << ng.getName() << "[0] * sizeof(unsigned int), cudaMemcpyHostToDevice));" << std::endl;
        }
    }
}
//--------------------------------------------------------------------------
void CUDA::genCurrentSpikePull(CodeStream &os, const NeuronGroup &ng, bool spikeEvent) const
{
    // Is push required at all
    const bool pullRequired = spikeEvent ?
        (ng.isSpikeEventRequired() && canPushPullVar(ng.getSpikeEventLocation()))
        : canPushPullVar(ng.getSpikeLocation());

    // Is delay required
    const bool delayRequired = spikeEvent ?
        ng.isDelayRequired() :
        (ng.isTrueSpikeRequired() && ng.isDelayRequired());

    const char *spikeCntPrefix = spikeEvent ? "glbSpkCntEvnt" : "glbSpkCnt";
    const char *spikePrefix = spikeEvent ? "glbSpkEvnt" : "glbSpk";

    if(pullRequired) {
        if (delayRequired) {
            os << "CHECK_CUDA_ERRORS(cudaMemcpy(" << spikeCntPrefix << ng.getName() << " + spkQuePtr" << ng.getName();
            os << ", d_" << spikeCntPrefix << ng.getName() << " + spkQuePtr" << ng.getName();
            os << ", sizeof(unsigned int), cudaMemcpyDeviceToHost));" << std::endl;

            os << "CHECK_CUDA_ERRORS(cudaMemcpy(" << spikePrefix << ng.getName() << " + (spkQuePtr" << ng.getName() << " * " << ng.getNumNeurons() << ")";
            os << ", d_" << spikePrefix << ng.getName() << " + (spkQuePtr" << ng.getName() << " * " << ng.getNumNeurons() << ")";
            os << ", " << spikeCntPrefix << ng.getName() << "[spkQuePtr" << ng.getName() << "] * sizeof(unsigned int), cudaMemcpyDeviceToHost));" << std::endl;
        }
        else {
            os << "CHECK_CUDA_ERRORS(cudaMemcpy(" << spikeCntPrefix << ng.getName();
            os << ", d_" << spikeCntPrefix << ng.getName();
            os << ", sizeof(unsigned int), cudaMemcpyDeviceToHost));" << std::endl;
            os << "CHECK_CUDA_ERRORS(cudaMemcpy(" << spikePrefix << ng.getName();
            os << ", d_" << spikePrefix << ng.getName();
            os << ", " << spikeCntPrefix << ng.getName() << "[0] * sizeof(unsigned int), cudaMemcpyDeviceToHost));" << std::endl;
        }
    }
}
//--------------------------------------------------------------------------
void CUDA::genPresynapticUpdatePreSpan(CodeStream &os, const NNmodel &model, const SynapseGroup &sg, const Substitutions &popSubs, bool trueSpike,
                                       SynapseGroupHandler wumThreshHandler, SynapseGroupHandler wumSimHandler) const
{
    // Get suffix based on type of events
    const std::string eventSuffix = trueSpike ? "" : "evnt";
    const auto *wu = sg.getWUModel();

    os << "if (" << popSubs.getVarSubstitution("id") << " < " ;
    if (sg.getSrcNeuronGroup()->isDelayRequired()) {
        os << "dd_glbSpkCnt" << eventSuffix << sg.getSrcNeuronGroup()->getName() << "[delaySlot])";
    }
    else {
        os << "dd_glbSpkCnt" << eventSuffix << sg.getSrcNeuronGroup()->getName() << "[0])";
    }
    {
        CodeStream::Scope b(os);

        if (!wu->getSimSupportCode().empty()) {
            os << "using namespace " << sg.getName() << "_weightupdate_simCode;" << std::endl;
        }

        if (sg.getSrcNeuronGroup()->isDelayRequired()) {
            os << "const unsigned int preInd = dd_glbSpk"  << eventSuffix << sg.getSrcNeuronGroup()->getName();
            os << "[(delaySlot * " << sg.getSrcNeuronGroup()->getNumNeurons() << ") + " << popSubs.getVarSubstitution("id") << "];" << std::endl;
        }
        else {
            os << "const unsigned int preInd = dd_glbSpk"  << eventSuffix << sg.getSrcNeuronGroup()->getName();
            os << "[" << popSubs.getVarSubstitution("id") << "];" << std::endl;
        }

        if(sg.getMatrixType() & SynapseMatrixConnectivity::YALE) {
            os << "unsigned int synAddress = dd_indInG" << sg.getName() << "[preInd];" << std::endl;
            os << "const unsigned int npost = dd_indInG" << sg.getName() << "[preInd + 1] - prePos;" << std::endl;
        }
        else if(sg.getMatrixType() & SynapseMatrixConnectivity::RAGGED) {
            os << "unsigned int synAddress = preInd * " << std::to_string(sg.getMaxConnections()) << ";" << std::endl;
            os << "const unsigned int npost = dd_rowLength" << sg.getName() << "[preInd];" << std::endl;
        }

        if (!trueSpike && sg.isEventThresholdReTestRequired()) {
            os << "if(";
 
            Substitutions threshSubs(&popSubs);
            threshSubs.addVarSubstitution("id_pre", "preInd");
            threshSubs.addVarSubstitution("id_post", "i");

            // Generate weight update threshold condition
            wumThreshHandler(os, sg, threshSubs);
            
            // end code substitutions ----
            os << ")";

            os << CodeStream::OB(130);
        }

        os << "for(unsigned int i = 0; i < npost; i++, synAddress++)";
        {
            CodeStream::Scope b(os);

            // **TODO** pretty sure __ldg will boost performance here - basically will bring whole row into cache
            os << "const unsigned int ipost = dd_ind" <<  sg.getName() << "[prePos];" << std::endl;

            // Code substitutions ----------------------------------------------------------------------------------
            std::string wCode = trueSpike ? wu->getSimCode() : wu->getEventCode();

            Substitutions synSubs(&popSubs);
            synSubs.addVarSubstitution("id_pre", "preInd");
            synSubs.addVarSubstitution("id_post", "ipost");
            synSubs.addVarSubstitution("id_syn", "synAddress");

            // If dendritic delay is required, always use atomic operation to update dendritic delay buffer
            if(sg.isDendriticDelayRequired()) {
                synSubs.addFuncSubstitution("addToInSynDelay", 2, getFloatAtomicAdd(model.getPrecision()) + "(&dd_denDelay" + sg.getPSModelTargetName() + "[" + sg.getDendriticDelayOffset("dd_", "$(1)") + "ipost], $(0))");
            }
            // Otherwise
            else {
                // If postsynaptic input should be accumulated in shared memory, substitute shared memory array for $(inSyn)
                if(shouldAccumulateInSharedMemory(sg)) {
                    synSubs.addFuncSubstitution("addToInSyn", 1, getFloatAtomicAdd(model.getPrecision()) + "(&shLg[ipost], $(0))");
                }
                // Otherwise, substitute global memory array for $(inSyn)
                else {
                    synSubs.addFuncSubstitution("addToInSyn", 1, getFloatAtomicAdd(model.getPrecision()) + "(&dd_inSyn" + sg.getPSModelTargetName() + "[ipost], $(0))");
                }
            }

            wumSimHandler(os, sg, synSubs);
        }

        if (!trueSpike && sg.isEventThresholdReTestRequired()) {
            os << CodeStream::CB(130);
        }
    }
}
//--------------------------------------------------------------------------
void CUDA::genPresynapticUpdatePostSpan(CodeStream &os, const NNmodel &model, const SynapseGroup &sg, const Substitutions &popSubs, bool trueSpike,
                                        SynapseGroupHandler wumThreshHandler, SynapseGroupHandler wumSimHandler) const
{
     // Get suffix based on type of events
    const std::string eventSuffix = trueSpike ? "" : "Evnt";

    os << "const unsigned int numSpikes = dd_glbSpkCnt" << eventSuffix << sg.getSrcNeuronGroup()->getName();
    if (sg.getSrcNeuronGroup()->isDelayRequired()) {
        os << "[preReadDelaySlot];" << std::endl;
    }
    else {
        os << "[0];" << std::endl;
    }
    os << "const unsigned int numSpikeBlocks = (numSpikes + " << m_KernelBlockSizes[KernelPresynapticUpdate] << " - 1) / " << m_KernelBlockSizes[KernelPresynapticUpdate] << ";" << std::endl;


    const auto *wu = sg.getWUModel();
    os << "for (unsigned int r = 0; r < numSpikeBlocks; r++)";
    {
        CodeStream::Scope b(os);
        os << "const unsigned int numSpikesInBlock = (r == numSpikeBlocks - 1) ? ((numSpikes - 1) % " << m_KernelBlockSizes[KernelPresynapticUpdate] << ") + 1 : " << m_KernelBlockSizes[KernelPresynapticUpdate] << ";" << std::endl;
        
        os << "__syncthreads();" << std::endl;
        os << "if (threadIdx.x < numSpikesInBlock)";
        {
            CodeStream::Scope b(os);
            const std::string offsetTrueSpkPost = (sg.getTrgNeuronGroup()->isTrueSpikeRequired() && sg.getTrgNeuronGroup()->isDelayRequired()) ? "postReadDelayOffset + " : "";
            os << "const unsigned int spk = dd_glbSpk" << eventSuffix << sg.getSrcNeuronGroup()->getName() << "[" << offsetTrueSpkPost << "(r * " << m_KernelBlockSizes[KernelPresynapticUpdate] << ") + threadIdx.x];" << std::endl;
            os << "shSpk" << eventSuffix << "[threadIdx.x] = spk;" << std::endl;
            if(sg.getMatrixType() & SynapseMatrixConnectivity::RAGGED) {
                os << "shRowLength" << eventSuffix << "[threadIdx.x] = dd_rowLength" << sg.getName() << "[spk];" << std::endl;
            }
        }
        os << "__syncthreads();" << std::endl;

        os << "// loop through all incoming spikes" << std::endl;
        os << "for (unsigned int j = 0; j < numSpikesInBlock; j++)";
        {
            CodeStream::Scope b(os);
            os << "// only work on existing neurons" << std::endl;
            os << "if (" << popSubs.getVarSubstitution("id") << " < " << sg.getMaxConnections() << ")";
            {
                CodeStream::Scope b(os);
                if (sg.getMatrixType() & SynapseMatrixConnectivity::BITMASK) {
                    const size_t maxSynapses = (size_t)sg.getTrgNeuronGroup()->getNumNeurons() * (size_t)sg.getSrcNeuronGroup()->getNumNeurons();
                    if((maxSynapses & 0xFFFFFFFF00000000ULL) != 0) {
                        os << "const uint64_t gid = (shSpk" << eventSuffix << "[j] * " << sg.getTrgNeuronGroup()->getNumNeurons() << "ull + " << popSubs.getVarSubstitution("id") << ");" << std::endl;
                    }
                    else {
                        os << "const unsigned int gid = (shSpk" << eventSuffix << "[j] * " << sg.getTrgNeuronGroup()->getNumNeurons() << " + " << popSubs.getVarSubstitution("id") << ");" << std::endl;
                    }
                }

                if (!wu->getSimSupportCode().empty()) {
                    os << "using namespace " << sg.getName() << "_weightupdate_simCode;" << std::endl;
                }
                if (!trueSpike && sg.isEventThresholdReTestRequired()) {
                    os << "if(";
                    if (sg.getMatrixType() & SynapseMatrixConnectivity::BITMASK) {
                        // Note: we will just access global mem. For compute >= 1.2 simultaneous access to same global mem in the (half-)warp will be coalesced - no worries
                        os << "(B(dd_gp" << sg.getName() << "[gid / 32], gid & 31)) && ";
                    }

                    Substitutions threshSubs(&popSubs);
                    threshSubs.addVarSubstitution("id_pre", "preInd");
                    threshSubs.addVarSubstitution("id_post", "ipost");
                   
                    // Generate weight update threshold condition
                    wumThreshHandler(os, sg, threshSubs);

                    // end code substitutions ----
                    os << ")";
                    os << CodeStream::OB(130);
                }
                else if (sg.getMatrixType() & SynapseMatrixConnectivity::BITMASK) {
                    os << "if (B(dd_gp" << sg.getName() << "[gid / 32], gid & 31))" << CodeStream::OB(135);
                }


                if(sg.getMatrixType() & SynapseMatrixConnectivity::SPARSE) {
                    if (sg.getMatrixType() & SynapseMatrixConnectivity::YALE) {
                        os << "unsigned int synAddress = dd_indInG" << sg.getName() << "[shSpk" << eventSuffix << "[j]];" << std::endl;
                        os << "const unsigned int npost = dd_indInG" << sg.getName() << "[shSpk" << eventSuffix << "[j] + 1] - synAddress;" << std::endl;
                    }
                    else {
                        os << "unsigned int synAddress = shSpk" << eventSuffix << "[j] * " << std::to_string(sg.getMaxConnections()) << ";" << std::endl;
                        os << "const unsigned int npost = shRowLength" << eventSuffix << "[j];" << std::endl;
                    }

                    os << "if (" << popSubs.getVarSubstitution("id") << " < npost)" << CodeStream::OB(140);
                    os << "synAddress += " << popSubs.getVarSubstitution("id") << ";" << std::endl;
                    os << "const unsigned int ipost = dd_ind" << sg.getName() << "[synAddress];" << std::endl;
                }
                else { // DENSE
                    os << "ipost = " << popSubs.getVarSubstitution("id") << ";" << std::endl;
                }

                Substitutions synSubs(&popSubs);
                synSubs.addVarSubstitution("id_pre", "shSpk" + eventSuffix + "[j]");
                synSubs.addVarSubstitution("id_post", "ipost");
                synSubs.addVarSubstitution("id_syn", "synAddress");

                // If dendritic delay is required, always use atomic operation to update dendritic delay buffer
                if(sg.isDendriticDelayRequired()) {
                    synSubs.addFuncSubstitution("addToInSynDelay", 2, getFloatAtomicAdd(model.getPrecision()) + "(&dd_denDelay" + sg.getPSModelTargetName() + "[" + sg.getDendriticDelayOffset("dd_", "$(1)") + "ipost], $(0))");
                }
                // Otherwise
                else {
                    if (sg.getMatrixType() & SynapseMatrixConnectivity::SPARSE) { // SPARSE
                        // **THINK** this is only correct if there are no multapses i.e. there is only one synapse between any pair of pre and postsynaptic neurons
                        if (shouldAccumulateInSharedMemory(sg)) {
                            synSubs.addFuncSubstitution("addToInSyn", 1, getFloatAtomicAdd(model.getPrecision()) + "(&shLg[ipost], $(0))");
                        }
                        else {
                            synSubs.addFuncSubstitution("addToInSyn", 1, getFloatAtomicAdd(model.getPrecision()) + "(&dd_inSyn" + sg.getPSModelTargetName() + "[ipost], $(0))");
                        }
                    }
                    else {
                        synSubs.addFuncSubstitution("addToInSyn", 1, "linSyn += $(0)");
                    }
                }

                wumSimHandler(os, sg, synSubs);

                if (sg.getMatrixType() & SynapseMatrixConnectivity::SPARSE) {
                    os << CodeStream::CB(140); // end if (id < npost)
                }

                if (!trueSpike && sg.isEventThresholdReTestRequired()) {
                    os << CodeStream::CB(130); // end if (eCode)
                }
                else if (sg.getMatrixType() & SynapseMatrixConnectivity::BITMASK) {
                    os << CodeStream::CB(135); // end if (B(dd_gp" << sg.getName() << "[gid / 32], gid
                }
            }
        }
    }
}
//--------------------------------------------------------------------------
void CUDA::genKernelDimensions(CodeStream &os, Kernel kernel, size_t numThreads) const
{
    // Calculate grid size
    const size_t gridSize = ceilDivide(numThreads, m_KernelBlockSizes[kernel]);
    os << "const dim3 threads(" << m_KernelBlockSizes[kernel] << ", 1);" << std::endl;

    if (gridSize < getChosenCUDADevice().maxGridSize[1]) {
        os << "const dim3 grid(" << gridSize << ", 1);" << std::endl;
    }
    else {
        // **TODO** this needs to be implemented in genParallelGroup
        assert(false);
        const size_t squareGridSize = (size_t)std::ceil(std::sqrt(gridSize));
        os << "const dim3 grid(" << squareGridSize << ", "<< squareGridSize <<");" << std::endl;
    }
}
//--------------------------------------------------------------------------
bool CUDA::shouldAccumulateInLinSyn(const SynapseGroup &sg) const
{
    // We should accumulate each postsynaptic neuron's input in a register if matrix is dense or bitfield (where each thread represents an individual neuron)
    return ((sg.getMatrixType() & SynapseMatrixConnectivity::DENSE) || (sg.getMatrixType() & SynapseMatrixConnectivity::BITMASK));
}
//--------------------------------------------------------------------------
bool CUDA::shouldAccumulateInSharedMemory(const SynapseGroup &sg) const
{
    // If parallelism is presynaptic i.e. atomics are required and device is older than Maxwell, we shouldn't use shared memory as atomics are emulated
    // and actually slower than global memory (see https://devblogs.nvidia.com/gpu-pro-tip-fast-histograms-using-shared-atomics-maxwell/)
    if(sg.getSpanType() == SynapseGroup::SpanType::PRESYNAPTIC && getChosenCUDADevice().major < 5) {
        return false;
    }
    // Otherwise, we should accumulate each postsynaptic neuron's input in shared menory if matrix is sparse
    // and the output population is small enough that input to it can be stored in a shared memory array
    else {
        return ((sg.getMatrixType() & SynapseMatrixConnectivity::SPARSE) && sg.getTrgNeuronGroup()->getNumNeurons() <= m_KernelBlockSizes[KernelPresynapticUpdate]);
    }
}
//--------------------------------------------------------------------------
std::string CUDA::getFloatAtomicAdd(const std::string &ftype) const
{
    int version;
    cudaRuntimeGetVersion(&version);
    if (((getChosenCUDADevice().major < 2) && (ftype == "float"))
        || (((getChosenCUDADevice().major < 6) || (version < 8000)) && (ftype == "double"))) {
        return "atomicAddSW";
    }
    else {
        return "atomicAdd";
    }
}
}   // namespace Backends
}   // namespace CodeGenerator