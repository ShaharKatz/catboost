#include "perftest_module.h"
#include <catboost/libs/algo/features_data_helpers.h>

#include <catboost/libs/data_new/load_data.h>

#include <catboost/libs/logging/logging.h>

#include <library/json/json_value.h>
#include <library/getopt/small/last_getopt.h>
#include <library/threading/future/async.h>

#include <util/system/info.h>
#include <util/generic/algorithm.h>
#include <util/system/hp_timer.h>
#include <util/thread/pool.h>


struct TCMDOptions {
    TString PoolPath;
    TString CdPath;
    TString ModelPath;
    size_t BlockSize = Max<size_t>();
    size_t RepetitionCount = 1;
};

struct TTimingResult {
    TVector<double> Times;

    void Add(double val) {
        Times.push_back(val);
    }

    double Min() const {
        return *MinElement(Times.begin(), Times.end());
    }

    double Max() const {
        return *MaxElement(Times.begin(), Times.end());
    }

    double Mean() const {
        double sum = 0;
        for (auto t : Times) {
            sum += t;
        }
        return sum / Times.size();
    }

    void Output(const TTimingResult* ref = nullptr) const {
        auto myMin = Min();
        CATBOOST_INFO_LOG << "min:\t" << myMin;
        if (ref) {
            CATBOOST_INFO_LOG << "\t" << myMin/ref->Min();
        }
        CATBOOST_INFO_LOG << Endl;

        auto myMax = Max();
        CATBOOST_INFO_LOG << "max:\t" << myMax;
        if (ref) {
            CATBOOST_INFO_LOG << "\t" << myMax/ref->Max();
        }
        CATBOOST_INFO_LOG << Endl;

        auto mymean = Mean();
        CATBOOST_INFO_LOG << "mean:\t" << mymean;
        if (ref) {
            CATBOOST_INFO_LOG << "\t" << mymean / ref->Mean();
        }
        CATBOOST_INFO_LOG << Endl;
    }

    NJson::TJsonValue GetJsonValue() const {
        NJson::TJsonValue result;
        result["min"] = Min();
        result["max"] = Max();
        result["mean"] = Mean();
        return result;
    }
};

struct TResults {
    TMap<TString, THolder<TTimingResult>> Results;
    TString BaseResultName;

    void UpdateResult(const TString& name, double time) {
        auto& value = Results[name];
        if (!value) {
            value = MakeHolder<TTimingResult>();
        }
        value->Add(time);
    }

    void OutputResults() const {
        NJson::TJsonValue jsonValue;
        const TTimingResult* refTimingResult = nullptr;
        CATBOOST_INFO_LOG << "name\tvalue\tdiff" << Endl;

        if (BaseResultName) {
            CATBOOST_INFO_LOG << BaseResultName << "\t" << Endl;
            refTimingResult = Results.at(BaseResultName).Get();
            Results.at(BaseResultName)->Output(refTimingResult);
            jsonValue[BaseResultName] = refTimingResult->GetJsonValue();
        }
        for (const auto& [key, value] : Results) {
            if (key == BaseResultName) {
                continue;
            }
            CATBOOST_INFO_LOG << key << "\t" << Endl;
            value->Output(refTimingResult);
            jsonValue[key] = value->GetJsonValue();
        }
        TFileOutput resultsFile("results.json");
        resultsFile << jsonValue.GetStringRobust();
    }
};

class TCanonData {
public:
    static constexpr float Epsilon = 1e-6;
    TCanonData(size_t blockCount) {
        Results.resize(blockCount);
    }

    void CheckOrSet(size_t blockId, const TVector<double>& blockResult) {
        if (Results[blockId].empty()) {
            Results[blockId] = blockResult;
        }
        const auto& ref = Results[blockId];
        CB_ENSURE(blockResult.size() == ref.size());
        for (size_t i = 0; i < blockResult.size(); ++i) {
            if(abs(blockResult[i] - ref[i]) > Epsilon) {
                Cerr << LabeledDump(blockId, i, blockResult[i], ref[i], blockResult[i] - ref[i]) << Endl;
            }
        }
    }
private:
    TVector<TVector<double>> Results;
};

int DoMain(int argc, char** argv) {
    TCMDOptions options;
    auto parser = NLastGetopt::TOpts();
    parser.AddLongOption('f', "pool-path")
        .StoreResult(&options.PoolPath)
        .Required();
    parser.AddLongOption("cd")
        .StoreResult(&options.CdPath)
        .Required();;
    parser.AddLongOption('m', "model-path")
        .StoreResult(&options.ModelPath)
        .Required();
    parser.AddLongOption("block-size")
        .StoreResult(&options.BlockSize)
        .Optional();
    parser.AddLongOption("repetitions")
        .StoreResult(&options.RepetitionCount)
        .Optional();
    NLastGetopt::TOptsParseResult parserResult{&parser, argc, argv};
    TFullModel model = ReadModel(options.ModelPath);
    NCatboostOptions::TDsvPoolFormatParams dsvPoolFormatParams;
    dsvPoolFormatParams.CdFilePath = NCB::TPathWithScheme(options.CdPath, "dsv");
    NCB::TDataProviderPtr dataset = NCB::ReadDataset(
        NCB::TPathWithScheme(options.PoolPath, "dsv"),
        NCB::TPathWithScheme(),
        NCB::TPathWithScheme(),
        NCB::TPathWithScheme(),
        dsvPoolFormatParams,
        TVector<ui32>(),
        NCB::EObjectsOrder::Undefined,
        NSystemInfo::CachedNumberOfCpus(),
        true,
        /*classNames*/ Nothing()
    );

    const auto* rawObjectsData = dynamic_cast<const NCB::TRawObjectsDataProvider*>(dataset->ObjectsData.Get());
    CB_ENSURE(rawObjectsData, "Not supported for quantized pools");
    const ui32 consecutiveSubsetBegin = NCB::GetConsecutiveSubsetBegin(*rawObjectsData);
    auto getFeatureDataBeginPtr = [&](ui32 flatFeatureIdx) -> const float* {
        return NCB::GetRawFeatureDataBeginPtr(
            *rawObjectsData,
            consecutiveSubsetBegin,
            flatFeatureIdx);
    };


    options.BlockSize = Min(options.BlockSize, (size_t)dataset->GetObjectCount());
    Y_ENSURE(options.BlockSize > 0, "Empty pool");
    const size_t docsCount = dataset->GetObjectCount();
    const size_t blockCount = (docsCount) / options.BlockSize;
    const size_t factorsCount = (size_t)dataset->MetaInfo.GetFeatureCount();

    CATBOOST_DEBUG_LOG << "Blocks count: " << blockCount << " block size: " << options.BlockSize << Endl;

    TVector<TVector<TVector<float>>> nonTransposedPool(blockCount);
    TVector<TVector<TConstArrayRef<float>>> nonTranspFactorsRef(blockCount);
    TVector<TVector<TConstArrayRef<float>>> transpFactorsRef(blockCount);

    for(size_t blockId = 0; blockId < blockCount; ++blockId) {
        const size_t blockStart = options.BlockSize * blockId;
        const size_t docsInCurrBlock = Min<size_t>(options.BlockSize, docsCount - options.BlockSize * blockId);
        CB_ENSURE(docsInCurrBlock >= 0);
        transpFactorsRef[blockId].resize(factorsCount);
        for (size_t i = 0; i < factorsCount; ++i) {
            transpFactorsRef[blockId][i] = MakeArrayRef<const float>(
                getFeatureDataBeginPtr(i) + blockStart,
                docsInCurrBlock);
        }

        nonTransposedPool[blockId].resize(docsInCurrBlock);
        nonTranspFactorsRef[blockId].resize(docsInCurrBlock);
        for (size_t docId = 0; docId < docsInCurrBlock; ++docId) {
            auto &docFacs = nonTransposedPool[blockId][docId];
            docFacs.resize(factorsCount);
            for (size_t featureId = 0; featureId < factorsCount; ++featureId) {
                docFacs[featureId] = getFeatureDataBeginPtr(featureId)[blockStart + docId];
            }
            nonTranspFactorsRef[blockId][docId] = MakeArrayRef(docFacs);
        }
    }
    TVector<double> results_vec(options.BlockSize);
    TResults results;
    TCanonData canonData(blockCount);
    TVector<THolder<IPerftestModule>> modules;
    TSet<TPerftestModuleFactory::TKey> allRegisteredKeys;
    TPerftestModuleFactory::GetRegisteredKeys(allRegisteredKeys);
    int biggestPriority = Min<int>();
    for (const auto& key : allRegisteredKeys) {
        try {
            modules.emplace_back(TPerftestModuleFactory::Construct(key, model));
            if (modules.back()->GetComparisonPriority(IPerftestModule::EPerftestModuleDataLayout::ObjectsFirst) > biggestPriority) {
                biggestPriority = modules.back()->GetComparisonPriority(IPerftestModule::EPerftestModuleDataLayout::ObjectsFirst);
                results.BaseResultName = modules.back()->GetName(IPerftestModule::EPerftestModuleDataLayout::ObjectsFirst);
            }
            if (modules.back()->GetComparisonPriority(IPerftestModule::EPerftestModuleDataLayout::FeaturesFirst) > biggestPriority) {
                biggestPriority = modules.back()->GetComparisonPriority(IPerftestModule::EPerftestModuleDataLayout::FeaturesFirst);
                results.BaseResultName = modules.back()->GetName(IPerftestModule::EPerftestModuleDataLayout::FeaturesFirst);
            }
        } catch (yexception& e) {
            Cerr << "Failed to construct module " << key << " got error: " << e.what() << Endl;
        } catch (...) {
            Cerr << "Failed to construct module " << key << " got error: " << CurrentExceptionMessage() << Endl;
        }
    }
    for (size_t i = 0; i < options.RepetitionCount; ++i) {
        for (auto& module : modules) {
            if (module->SupportsLayout(IPerftestModule::EPerftestModuleDataLayout::ObjectsFirst)) {
                for (size_t blockId = 0; blockId < blockCount; ++blockId) {
                    results.UpdateResult(
                        module->GetName(IPerftestModule::EPerftestModuleDataLayout::ObjectsFirst),
                        module->Do(IPerftestModule::EPerftestModuleDataLayout::ObjectsFirst,
                                   nonTranspFactorsRef[blockId]));
                }
            }
            if (module->SupportsLayout(IPerftestModule::EPerftestModuleDataLayout::FeaturesFirst)) {
                for (size_t blockId = 0; blockId < blockCount; ++blockId) {
                    results.UpdateResult(
                        module->GetName(IPerftestModule::EPerftestModuleDataLayout::ObjectsFirst),
                        module->Do(IPerftestModule::EPerftestModuleDataLayout::FeaturesFirst,
                                   transpFactorsRef[blockId]));
                }
            }
        }
    }
    results.OutputResults();

    return 0;
}

int main(int argc, char** argv) {
    try {
        auto queue = CreateThreadPool(1);

        NThreading::TFuture<void> future = NThreading::Async(
            [=](){
                TSetLoggingVerbose inThisScope;
                DoMain(argc, argv);
            },
            *queue
        );
        future.GetValueSync();
    } catch (...) {
        Cerr << CurrentExceptionMessage() << Endl;
        return -1;
    }
    return 0;
}