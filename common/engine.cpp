/**
 * Basic TensorRT engine API based on ONNX parser.
 * 2020/09/01
 */
#include "engine.h"

#include <string>
#include <vector>
#include <iostream>
#include <cassert>
#include <fstream>
#include <memory>

#include "NvInfer.h"
#include "NvCaffeParser.h"
#include "NvOnnxParser.h"
#include "NvUffParser.h"
#include "NvInferPlugin.h"
#include "utils.h"

using namespace nvinfer1;


RTEngine::RTEngine() {}

RTEngine::~RTEngine() {
    if (mPluginFactory != nullptr) {
        delete mPluginFactory;
        mPluginFactory = nullptr;
    }
    if (mContext != nullptr) {
        mContext->destroy();
        mContext = nullptr;
    }
    if (mEngine !=nullptr) {
        mEngine->destroy();
        mEngine = nullptr;
    }
    for (size_t i=0;i<mBinding.size();i++) {
        safeCudaFree(mBinding[i]);
    }
}

void RTEngine::CreateEngine(const std::string& prototxt,
                            const std::string& caffeModel,
                            const std::string& engineFile,
                            const std::vector<std::string>& outputBlobName,
                            const std::vector<std::vector<float>>& calibratorData,
                            int maxBatchSize,
                            int mode) {
    mRunMode = mode;
    mInfoLogger.logger("prototxt: ",prototxt);
    mInfoLogger.logger("caffeModel: ",caffeModel);
    mInfoLogger.logger("engineFile: ",engineFile);
    mInfoLogger.logger("outputBlobName: ");
    for (size_t i=0;i<outputBlobName.size();i++) {
        std::cout << outputBlobName[i] << " ";
    }
    std::cout << std::endl;
    if (!DeserializeEngine(engineFile)) {
        if (!BuildEngine(prototxt,caffeModel,engineFile,outputBlobName,calibratorData,maxBatchSize)) {
            mInfoLogger.logger("error: could not deserialize or build engine");
            return;
        }
    }
    mInfoLogger.logger("Create execute context and malloc device memory...");
    mFromOnnx = false;
    InitEngine();
    // Notice: close profiler
    //mContext->setProfiler(mProfiler);
}

void RTEngine::CreateEngine(const std::string& onnxModel,
                            const std::string& engineFile,
                            const std::vector<std::string>& customOutput,
                            int maxBatchSize,
                            RunMode runMode,
                            long workspace_size) {
    if (!DeserializeEngine(engineFile)) {
        if (!BuildEngine(onnxModel,engineFile,customOutput,maxBatchSize, runMode, workspace_size)) {
            mInfoLogger.logger("error: could not deserialize or build engine");
            return;
        }
    }
    mInfoLogger.logger("Create execute context and malloc device memory...");
    InitEngine();
}

void RTEngine::CreateEngine(const std::string& uffModel,
                            const std::string& engineFile,
                            const std::vector<std::string>& inputTensorNames,
                            const std::vector<std::vector<int>>& inputDims,
                            const std::vector<std::string>& outputTensorNames,
                            int maxBatchSize) {
    if (!DeserializeEngine(engineFile)) {
        if (!BuildEngine(uffModel,engineFile,inputTensorNames,inputDims, outputTensorNames,maxBatchSize)) {
            mInfoLogger.logger("could not deserialize or build engine", logger::LEVEL::ERROR);
            return;
        }
    }
    mInfoLogger.logger("create execute context and malloc device memory...");
    InitEngine();
}

void RTEngine::Forward() {
//     cudaEvent_t start,stop;
//     float elapsedTime;
//     cudaEventCreate(&start);
//     cudaEventCreate(&stop);
//     cudaEventRecord(start, 0);
    mContext->execute(mBatchSize, &mBinding[0]);
//     cudaEventRecord(stop, 0);
//	 cudaEventSynchronize(stop);
//	 cudaEventElapsedTime(&elapsedTime, start, stop);
//     mInfoLogger.logger("net forward takes: ", elapsedTime, "ms");
}

void RTEngine::ForwardAsync(const cudaStream_t& stream) {
//    cudaEvent_t start,stop;
//    float elapsedTime;
//    cudaEventCreate(&start);
//    cudaEventCreate(&stop);
//    cudaEventRecord(start, 0);
    mContext->enqueue(mBatchSize, &mBinding[0], stream, nullptr);
//    cudaEventRecord(stop, 0);
//    cudaEventSynchronize(stop);
//    cudaEventElapsedTime(&elapsedTime, start, stop);
//    mInfoLogger.logger("net forward takes: ", elapsedTime, "ms");
}

void RTEngine::DataTransfer(std::vector<float>& data, int bindIndex, bool isHostToDevice) {
    if (isHostToDevice) {
        assert(data.size()*sizeof(float) == mBindingSize[bindIndex]);
        CUDA_CHECK(cudaMemcpy(mBinding[bindIndex], data.data(), mBindingSize[bindIndex], cudaMemcpyHostToDevice));
    } else {
        data.resize(mBindingSize[bindIndex]/sizeof(float));
        CUDA_CHECK(cudaMemcpy(data.data(), mBinding[bindIndex], mBindingSize[bindIndex], cudaMemcpyDeviceToHost));
    }
}

void RTEngine::DataTransferAsync(std::vector<float>& data, int bindIndex, bool isHostToDevice, cudaStream_t& stream) {
    if (isHostToDevice) {
        assert(data.size()*sizeof(float) == mBindingSize[bindIndex]);
        CUDA_CHECK(cudaMemcpyAsync(mBinding[bindIndex], data.data(), mBindingSize[bindIndex], cudaMemcpyHostToDevice, stream));
    } else {
        data.resize(mBindingSize[bindIndex]/sizeof(float));
        CUDA_CHECK(cudaMemcpyAsync(data.data(), mBinding[bindIndex], mBindingSize[bindIndex], cudaMemcpyDeviceToHost, stream));
    }
}

void RTEngine::CopyFromHostToDevice(const std::vector<float>& input, int bindIndex) {
    CUDA_CHECK(cudaMemcpy(mBinding[bindIndex], input.data(), mBindingSize[bindIndex], cudaMemcpyHostToDevice));
}

void RTEngine::CopyFromHostToDevice(const std::vector<float>& input, int bindIndex, const cudaStream_t& stream) {
    CUDA_CHECK(cudaMemcpyAsync(mBinding[bindIndex], input.data(), mBindingSize[bindIndex], cudaMemcpyHostToDevice, stream));
}

void RTEngine::CopyFromDeviceToHost(std::vector<float>& output, int bindIndex) {
    CUDA_CHECK(cudaMemcpy(output.data(), mBinding[bindIndex], mBindingSize[bindIndex], cudaMemcpyDeviceToHost));
}

void RTEngine::CopyFromDeviceToHost(std::vector<float>& output, int bindIndex, const cudaStream_t& stream) {
    CUDA_CHECK(cudaMemcpyAsync(output.data(), mBinding[bindIndex], mBindingSize[bindIndex], cudaMemcpyDeviceToHost, stream));
}

void RTEngine::SetDevice(int device) {
    mInfoLogger.logger("Make sure save engine file match choosed device.", logger::LEVEL::WARNING);
    CUDA_CHECK(cudaSetDevice(device));
}

int RTEngine::GetDevice() {
    int* device = nullptr; //NOTE: memory leaks here
    CUDA_CHECK(cudaGetDevice(device));
    if (device != nullptr) {
        return device[0];
    }
    else {
        mInfoLogger.logger("Get Device Error", logger::LEVEL::ERROR);
        return -1;
    }
}

int RTEngine::GetMaxBatchSize() const {
    return mBatchSize;
}

void* RTEngine::GetBindingPtr(int bindIndex) const {
    return mBinding[bindIndex];
}

size_t RTEngine::GetBindingSize(int bindIndex) const {
    return mBindingSize[bindIndex];
}

nvinfer1::Dims RTEngine::GetBindingDims(int bindIndex) const {
    return mBindingDims[bindIndex];
}

nvinfer1::DataType RTEngine::GetBindingDataType(int bindIndex) const {
    return mBindingDataType[bindIndex];
}

void RTEngine::SaveEngine(const std::string& fileName) {
    if (fileName == "") {
        mInfoLogger.logger("empty engine file name, skip save");
        return;
    }
    if (mEngine != nullptr) {
        mInfoLogger.logger("save engine to: ", fileName);
        nvinfer1::IHostMemory* data = mEngine->serialize();
        std::ofstream file;
        file.open(fileName,std::ios::binary | std::ios::out);
        if(!file.is_open()) {
            mInfoLogger.logger("read create engine file failed: ",fileName);
            return;
        }
        file.write((const char*)data->data(), data->size());
        file.close();
        data->destroy();
    } else {
        mInfoLogger.logger("engine is empty, save engine failed");
    }
}

bool RTEngine::DeserializeEngine(const std::string& engineFile) {
    std::ifstream in(engineFile.c_str(), std::ifstream::binary);
    if (in.is_open()) {
        mInfoLogger.logger("Deserialize engine from:", engineFile);
        auto const start_pos = in.tellg();
        in.ignore(std::numeric_limits<std::streamsize>::max());
        size_t bufCount = in.gcount();
        in.seekg(start_pos);
        std::unique_ptr<char[]> engineBuf(new char[bufCount]);
        in.read(engineBuf.get(), bufCount);
        initLibNvInferPlugins(&mLogger, "");
        mRuntime = nvinfer1::createInferRuntime(mLogger);
        mEngine = mRuntime->deserializeCudaEngine((void*)engineBuf.get(), bufCount, nullptr);
        assert(mEngine != nullptr);
        mBatchSize = mEngine->getMaxBatchSize();
        mInfoLogger.logger("Max batch size of deserialized engine:",mEngine->getMaxBatchSize());
        mRuntime->destroy();
        return true;
    }
    return false;
}

bool RTEngine::BuildEngine(const std::string& prototxt,
                           const std::string& caffeModel,
                           const std::string& engineFile,
                           const std::vector<std::string>& outputBlobName,
                           const std::vector<std::vector<float>>& calibratorData,
                           int maxBatchSize) {
        mBatchSize = maxBatchSize;
        mInfoLogger.logger("build caffe engine with: ", prototxt, caffeModel);
        nvinfer1::IBuilder* builder = nvinfer1::createInferBuilder(mLogger);
        assert(builder != nullptr);
        // NetworkDefinitionCreationFlag::kEXPLICIT_BATCH
        nvinfer1::INetworkDefinition* network = builder->createNetworkV2(0);
        assert(network != nullptr);
        nvcaffeparser1::ICaffeParser* parser = nvcaffeparser1::createCaffeParser();
        if (mPluginFactory != nullptr) {
//            parser->setPluginFactoryV2(mPluginFactory);
        }
        // Notice: change here to costom data type
        nvinfer1::DataType type = mRunMode==1 ? nvinfer1::DataType::kHALF : nvinfer1::DataType::kFLOAT;
        const nvcaffeparser1::IBlobNameToTensor* blobNameToTensor = parser->parse(prototxt.c_str(),caffeModel.c_str(),
                                                                                *network,type);

        for (auto& s : outputBlobName) {
            network->markOutput(*blobNameToTensor->find(s.c_str()));
        }
        mInfoLogger.logger("Number of network layers: ",network->getNbLayers());
        mInfoLogger.logger("Number of input: ", network->getNbInputs());
        std::cout << "Input layer: " << std::endl;
        for (int i = 0; i < network->getNbInputs(); i++) {
            std::cout << network->getInput(i)->getName() << " : ";
            Dims dims = network->getInput(i)->getDimensions();
            for (int j = 0; j < dims.nbDims; j++) {
                std::cout << dims.d[j] << "x";
            }
            std::cout << "\b "  << std::endl;
        }
        mInfoLogger.logger("Number of output: ",network->getNbOutputs());
        std::cout << "Output layer: " << std::endl;
        for (int i = 0; i < network->getNbOutputs(); i++) {
            std::cout << network->getOutput(i)->getName() << " : ";
            Dims dims = network->getOutput(i)->getDimensions();
            for(int j = 0; j < dims.nbDims; j++) {
                std::cout << dims.d[j] << "x";
            }
            std::cout << "\b " << std::endl;
        }
        mInfoLogger.logger("parse network done");
        nvinfer1::IBuilderConfig* config = builder->createBuilderConfig();

        IInt8EntropyCalibrator* calibrator = nullptr;
        if (mRunMode == 2)
        {
            mInfoLogger.logger("set int8 inference mode");
            if (!builder->platformHasFastInt8()) {
                mInfoLogger.logger("current platform doesn't support int8 inference", logger::LEVEL::WARNING);
            }
            if (calibratorData.size() > 0 ) {
                auto endPos= prototxt.find_last_of(".");
                auto beginPos= prototxt.find_last_of('/') + 1;
                if(prototxt.find("/") == std::string::npos) {
                    beginPos = 0;
                }
                std::string calibratorName = prototxt.substr(beginPos,endPos - beginPos);
                std::cout << "create calibrator,Named:" << calibratorName << std::endl;
//                calibrator = new IInt8EntropyCalibrator(maxBatchSize,calibratorData,calibratorName,false);
            }
            // enum class BuilderFlag : int
            // {
            //     kFP16 = 0,         //!< Enable FP16 layer selection.
            //     kINT8 = 1,         //!< Enable Int8 layer selection.
            //     kDEBUG = 2,        //!< Enable debugging of layers via synchronizing after every layer.
            //     kGPU_FALLBACK = 3, //!< Enable layers marked to execute on GPU if layer cannot execute on DLA.
            //     kSTRICT_TYPES = 4, //!< Enables strict type constraints.
            //     kREFIT = 5,        //!< Enable building a refittable engine.
            // };
            config->setFlag(nvinfer1::BuilderFlag::kINT8);
            config->setInt8Calibrator(calibrator);
        }

        if (mRunMode == 1)
        {
            mInfoLogger.logger("setFp16Mode");
            if (!builder->platformHasFastFp16()) {
                mInfoLogger.logger("the platform do not has fast for fp16");
            }
            config->setFlag(nvinfer1::BuilderFlag::kFP16);
        }
        builder->setMaxBatchSize(mBatchSize);
        // set the maximum GPU temporary memory which the engine can use at execution time.
        config->setMaxWorkspaceSize(10 << 20);

        mInfoLogger.logger("fp16 support: ",builder->platformHasFastFp16 ());
        mInfoLogger.logger("int8 support: ",builder->platformHasFastInt8 ());
        mInfoLogger.logger("Max batchsize: ",builder->getMaxBatchSize());
        mInfoLogger.logger("Max workspace size: ",config->getMaxWorkspaceSize());
        mInfoLogger.logger("Number of DLA core: ",builder->getNbDLACores());
        mInfoLogger.logger("Max DLA batchsize: ",builder->getMaxDLABatchSize());
        mInfoLogger.logger("Current use DLA core: ",config->getDLACore()); // TODO: set DLA core
        mInfoLogger.logger("build engine...");
        mEngine = builder -> buildEngineWithConfig(*network, *config);
        assert(mEngine != nullptr);
        mInfoLogger.logger("serialize engine to: ", engineFile);
        SaveEngine(engineFile);

        builder->destroy();
        config->destroy();
        network->destroy();
        parser->destroy();
        if (calibrator) {
            delete calibrator;
            calibrator = nullptr;
        }
        return true;
}

bool RTEngine::BuildEngine(const std::string& onnxModel,
                           const std::string& engineFile,
                           const std::vector<std::string>& customOutput,
                           int maxBatchSize,
                           RunMode runMode,
                           long workspace_size) {
    mInfoLogger.logger("The ONNX Parser shipped with TensorRT 5.1.x+ supports ONNX IR (Intermediate Representation) version 0.0.3, opset version 9");
    mBatchSize = maxBatchSize;
    mInfoLogger.logger("Build onnx engine from: ", onnxModel);
    nvinfer1::IBuilder* builder = nvinfer1::createInferBuilder(mLogger);
    assert(builder != nullptr && "Builder is NULL!");
    auto explicitBtach = 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
    nvinfer1::INetworkDefinition* network = builder->createNetworkV2(explicitBtach);
    assert(network != nullptr && "Network is NULL");
    nvonnxparser::IParser* parser = nvonnxparser::createParser(*network, mLogger);
    if (!parser->parseFromFile(onnxModel.c_str(), static_cast<int>(ILogger::Severity::kWARNING))) {
        mInfoLogger.logger("Could not parse onnx engine", logger::LEVEL::ERROR);
        return false;
    }
//    for(int i=0;i<network->getNbLayers();i++) {
//        nvinfer1::ILayer* custom_output = network->getLayer(i);
//        for(int j=0;j<custom_output->getNbInputs();j++) {
//            nvinfer1::ITensor* input_tensor = custom_output->getInput(j);
//            std::cout << input_tensor->getName() << " ";
//        }
//        std::cout << " -------> ";
//        for(int j=0;j<custom_output->getNbOutputs();j++) {
//            nvinfer1::ITensor* output_tensor = custom_output->getOutput(j);
//            std::cout << output_tensor->getName() << " ";
//        }
//        std::cout << std::endl;
//    }
//    if(customOutput.size() > 0) {
//        mInfoLogger.logger("unmark original output...");
//        for(int i=0;i<network->getNbOutputs();i++) {
//            nvinfer1::ITensor* origin_output = network->getOutput(i);
//            network->unmarkOutput(*origin_output);
//        }
//        mInfoLogger.logger("mark custom output...");
//        for(int i=0;i<network->getNbLayers();i++) {
//            nvinfer1::ILayer* custom_output = network->getLayer(i);
//            nvinfer1::ITensor* output_tensor = custom_output->getOutput(0);
//            for(size_t j=0; j<customOutput.size();j++) {
//                std::string layer_name(output_tensor->getName());
//                if(layer_name == customOutput[j]) {
//                    network->markOutput(*output_tensor);
//                    break;
//                }
//            }
//        }
//    }
    nvinfer1::IBuilderConfig* config = builder->createBuilderConfig();
    builder->setMaxBatchSize(mBatchSize);
    config->setMaxWorkspaceSize(workspace_size << 20);

    switch(runMode) {
        case(RunMode::kFP16): {
            if (!builder->platformHasFastFp16()) {
                mInfoLogger.logger("Device do not support FP16 mode, switch to fp32 mode.", logger::LEVEL::WARNING);
                break;
            }
            else {
                mInfoLogger.logger("Set engine to fp16 mode ");
                config->setFlag(nvinfer1::BuilderFlag::kFP16);
                break;
            }
        }
        case (RunMode::kINT8): {
            if (!builder->platformHasFastInt8()) {
                mInfoLogger.logger("Device do not support INT8 mode, switch to fp32 mode.", logger::LEVEL::WARNING);
                break;
            } else {
                mInfoLogger.logger("Set engine to int8 mode ");
                config->setFlag(nvinfer1::BuilderFlag::kINT8);
                setAllTensorScales(network, 127.0f, 127.0f);
    //            builder->setStrictTypeConstraints(true);
                break;
            }
        }
    }

    mEngine = builder -> buildEngineWithConfig(*network, *config);
    assert(mEngine != nullptr);
    mInfoLogger.logger("Serialize engine to: ", engineFile);
    SaveEngine(engineFile);

    builder->destroy();
    network->destroy();
    parser->destroy();
    return true;
}

bool RTEngine::BuildEngine(const std::string& uffModel,
                           const std::string& engineFile,
                           const std::vector<std::string>& inputTensorNames,
                           const std::vector<std::vector<int>>& inputDims,
                           const std::vector<std::string>& outputTensorNames,
                           int maxBatchSize) {
    mBatchSize = maxBatchSize;
    mInfoLogger.logger("build uff engine with: ", uffModel);
    nvinfer1::IBuilder* builder = nvinfer1::createInferBuilder(mLogger);
    assert(builder != nullptr);
    // NetworkDefinitionCreationFlag::kEXPLICIT_BATCH
    nvinfer1::INetworkDefinition* network = builder->createNetworkV2(0);
    assert(network != nullptr);
    nvuffparser::IUffParser* parser = nvuffparser::createUffParser();
    assert(parser != nullptr);
    assert(inputTensorNames.size() == inputDims.size());
    //parse input
    for (size_t i=0;i<inputTensorNames.size();i++) {
        nvinfer1::Dims dim;
        dim.nbDims = inputDims[i].size();
        for(int j=0;j<dim.nbDims;j++) {
            dim.d[j] = inputDims[i][j];
        }
        parser->registerInput(inputTensorNames[i].c_str(), dim, nvuffparser::UffInputOrder::kNCHW);
    }
    //parse output
    for (size_t i=0;i<outputTensorNames.size();i++) {
        parser->registerOutput(outputTensorNames[i].c_str());
    }
    if (!parser->parse(uffModel.c_str(), *network, nvinfer1::DataType::kFLOAT)) {
        mInfoLogger.logger("parse model failed", logger::LEVEL::ERROR);
    }
    nvinfer1::IBuilderConfig* config = builder->createBuilderConfig();
    config->setMaxWorkspaceSize(10 << 20);
    builder->setMaxBatchSize(mBatchSize);
    mEngine = builder -> buildEngineWithConfig(*network, *config);
    assert(mEngine != nullptr);
    mInfoLogger.logger("serialize engine to: ", engineFile);
    SaveEngine(engineFile);

    builder->destroy();
    network->destroy();
    parser->destroy();
    return true;
}

// void RTEngine::ParseUff(const std::string& uffModel) {
//     mInfoLogger.logger("parse uff model, this parser is very simple");
//     mInfoLogger.logger("I recommend you to see https://lutzroeder.github.io/netron/");
//     mInfoLogger.logger("You will get a more user-friendly output");
//     tensorflow::GraphDef graph;
//     fstream input(uffModel.c_str(), ios::in | ios::binary);
//     if (!graph.ParseFromIstream(&input)) {
//         mInfoLogger.logger("Failed to parse uff model");
//         return;
//     }
//     for(int i=0;i<graph.node_size();i++) {
//         tensorflow::NodeDef node = graph.node(i);
//         std::cout << "--------------------------------" << std::endl;
//         std::cout << node.name() << ", " << node.op() << std::endl;
//     }

// }

void RTEngine::InitEngine() {
    mInfoLogger.logger("Init engine...");
    mContext = mEngine->createExecutionContext();
    assert(mContext != nullptr);

    mInfoLogger.logger("Malloc device memory...");
    int nbBindings = mEngine->getNbBindings();
    mInfoLogger.logger("nbBingdings: ", nbBindings);
    mBinding.resize(nbBindings);
    mBindingSize.resize(nbBindings);
    mBindingName.resize(nbBindings);
    mBindingDims.resize(nbBindings);
    mBindingDataType.resize(nbBindings);
    for (int i=0; i< nbBindings; i++) {
        nvinfer1::Dims dims = mEngine->getBindingDimensions(i);
        nvinfer1::DataType dtype = mEngine->getBindingDataType(i);
        const char* name = mEngine->getBindingName(i);
        int64_t totalSize;
        if (mFromOnnx)
            totalSize = volume(dims) * getElementSize(dtype);
        else
            totalSize = volume(dims) * mBatchSize * getElementSize(dtype);
//        std::cout << "\n\ndims :: " << volume(dims) << "  batch_in_trt :: "<< mBatchSize << "  element size :: " << getElementSize(dtype) << "\n\n\n";
        mBindingSize[i] = totalSize;
        mBindingName[i] = name;
        mBindingDims[i] = dims;
        mBindingDataType[i] = dtype;
        if (mEngine->bindingIsInput(i)) {
            mInfoLogger.logger("Input: ");
        } else {
            mInfoLogger.logger("Output: ");
        }
        std::cout << "Binding bindIndex: " << i << ", Name: " << name << ", Size in bytes: " << totalSize << std::endl;
        std::cout << "Binding dims with " << dims.nbDims << " dimemsions" << std::endl;
        for (int j=0;j<dims.nbDims;j++) {
            std::cout << dims.d[j] << " x ";
        }
        std::cout << "\b\b  "<< std::endl;
        mBinding[i] = safeCudaMalloc(totalSize);
        if (mEngine->bindingIsInput(i)) {
            mInputSize++;
        }
    }
}