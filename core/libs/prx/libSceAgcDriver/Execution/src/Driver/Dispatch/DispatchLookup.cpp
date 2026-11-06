#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <cstdlib>

namespace AgcDriver::DriverDetail {

void Driver::lookupDispatch(std::uint64_t address, const Submission& submission, const std::shared_ptr<const ShaderSnapshot>& registeredShader, std::uint64_t key, std::span<const std::uint32_t> userData, bool noDispatchCache, bool traceCache, bool profile, std::span<const ShaderRecompiler::MemoryRegion> memory, DispatchPhaseTiming& phaseTiming, std::array<double, DriverPhaseCount>& phaseMs, std::shared_ptr<const ShaderRecompiler::RecompileResult>& compiledResult, std::shared_ptr<DispatchVariant>& keepVariant, std::vector<ShaderRecompiler::MemoryRegion>& captured, std::vector<std::uint32_t>& liveWords, bool& dataHit, bool& cached, bool& validated, std::shared_ptr<DispatchEntry>& missedEntry, bool& missedDiffering) {
    if (!noDispatchCache) {

        static const bool validateUnlocked = std::getenv("APS5_NO_UNLOCKED_VALIDATE") == nullptr;
        std::unique_lock cacheLock(dispatchCacheMutex);
        ++entryCounters.lookups;
        const auto found = dispatchCache.find(key);
        std::shared_ptr<DispatchEntry> entry = found != dispatchCache.end() ? found->second : nullptr;
        if (entry == nullptr) ++entryCounters.absent;
        if (validateUnlocked) cacheLock.unlock();
        phaseTiming.Phase(PhaseLookup);
        if (entry != nullptr) {
            validated = true;
            const auto& variants = entry->variants;

            std::shared_ptr<DispatchVariant> variant;
            std::shared_ptr<DispatchVariant> effectiveVariant;
            std::size_t rank = 0;
            const auto generation = variants.front()->generation.load(std::memory_order_acquire);
            bool current = false;
            std::uint64_t restamped = 0;

            auto outcome = EntryOutcome::Differing;
            std::uint64_t imagesFlushed = 0, runsSynced = 0, retriesEqual = 0, retriesMoved = 0, compared = 0;
            std::vector<ShaderRecompiler::MemoryRegion> regions;

            std::vector<std::pair<std::uint32_t, std::uint32_t>> liveData;
            const auto waitedBeforeValidate = phaseTiming.profile ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
            if (!stampValidate()) {
                const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DispatchCache);

                std::optional<SampledReadScope> sampling;

                for (std::size_t i = 0; i < variants.size(); ++i) {
                    const auto& donor = variants[i];
                    if (donor->shader != registeredShader) continue;
                    if (donor->userDataRoots.size() != userData.size()) continue;
                    std::uint64_t changedRoots = 0;
                    for (std::size_t root = 0; root < userData.size() && root < 64; ++root) {
                        if (donor->userDataRoots[root] != userData[root]) changedRoots |= std::uint64_t{1} << root;
                    }
                    if ((changedRoots & ~donor->relocatableRootMask) != 0) continue;
                    bool alignedRoots = true;
                    for (std::size_t root = 0; root < userData.size() && root < 64; ++root) {
                        if ((changedRoots & (std::uint64_t{1} << root)) != 0 && (donor->userDataRoots[root] == 0 || userData[root] == 0 || (donor->userDataRoots[root] & 3u) != 0 || (userData[root] & 3u) != 0)) alignedRoots = false;
                    }
                    if (!alignedRoots) continue;
                    std::shared_ptr<DispatchVariant> effective;
                    if (changedRoots != 0) {
                        if (stampValidate()) continue;
                        effective = std::make_shared<DispatchVariant>();
                        effective->runs = donor->runs;
                        effective->wordSource = donor;
                        effective->forgetSerial = donor->forgetSerial;
                        effective->compiled = donor->compiled;
                        effective->registeredViews = donor->registeredViews;
                        effective->shader = donor->shader;
                        effective->recipe.store(donor->recipe.load(std::memory_order_acquire), std::memory_order_release);
                        effective->dataPositions = donor->dataPositions;
                        effective->dataSlots = donor->dataSlots;
                        effective->flatBinding = donor->flatBinding;
                        effective->runBases = donor->runBases;
                        effective->userDataRoots.assign(userData.begin(), userData.end());
                        effective->relocatableRootMask = donor->relocatableRootMask;
                        bool mapped[64]{};
                        bool valid = donor->runBases.size() == donor->runs.size();
                        if (!valid) continue;
                        for (std::size_t run = 0; run < effective->runs.size(); ++run) {
                            const auto root = effective->runBases[run];
                            if (root == -1) continue;
                            if (root < 0 || root >= 64) {
                                valid = false;
                                break;
                            }
                            if ((changedRoots & (std::uint64_t{1} << root)) == 0) continue;
                            mapped[static_cast<std::size_t>(root)] = true;
                            const std::int64_t delta = static_cast<std::int64_t>(userData[static_cast<std::size_t>(root)]) - static_cast<std::int64_t>(donor->userDataRoots[static_cast<std::size_t>(root)]);
                            const auto shift = [&](std::uint64_t& addressValue) {
                                if (delta < 0) {
                                    const auto amount = static_cast<std::uint64_t>(-delta);
                                    if (addressValue < amount) return false;
                                    addressValue -= amount;
                                } else {
                                    const auto amount = static_cast<std::uint64_t>(delta);
                                    if (addressValue > UINT64_MAX - amount) return false;
                                    addressValue += amount;
                                }
                                return true;
                            };
                            if (!shift(effective->runs[run].first) || !shift(effective->runs[run].second) || effective->runs[run].second <= effective->runs[run].first) {
                                valid = false;
                                break;
                            }
                        }
                        for (std::size_t left = 0; left < effective->runs.size(); ++left) {
                            for (std::size_t right = 0; right < left; ++right) {
                                const auto& a = effective->runs[left];
                                const auto& b = effective->runs[right];
                                if (a.first < b.second && b.first < a.second) valid = false;
                            }
                        }
                        for (std::size_t root = 0; root < userData.size() && root < 64; ++root) {
                            if ((changedRoots & (std::uint64_t{1} << root)) != 0 && !mapped[root]) valid = false;
                        }
                        if (!valid) continue;
                    }
                    const auto& candidate = effective != nullptr ? *effective : *donor;
                    if (candidate.registeredViews != nullptr && !CaptureRegisteredViewsCurrent(*candidate.registeredViews)) continue;
                    regions.clear();
                    appendEntryRegions(candidate, regions);
                    ++compared;
                    auto result = validateVariant(address, submission.queue, candidate, regions, imagesFlushed, runsSynced, sampling, &liveData);

                    if (gateRetry() && (result == EntryOutcome::PublishMoved || result == EntryOutcome::PendingMoved)) {
                        result = validateVariant(address, submission.queue, candidate, regions, imagesFlushed, runsSynced, sampling, &liveData);
                        ++(result == EntryOutcome::Equal || result == EntryOutcome::EqualData ? retriesEqual : retriesMoved);
                    }
                    if (result == EntryOutcome::Equal || result == EntryOutcome::EqualData) {
                        outcome = result;
                        variant = donor;
                        effectiveVariant = std::move(effective);
                        rank = i;
                        dataHit = result == EntryOutcome::EqualData;
                        break;
                    }
                }
                current = variant != nullptr;
            } else {
                variant = variants.front();
                current = variant->shader == registeredShader && variant->userDataRoots.size() == userData.size() && std::equal(variant->userDataRoots.begin(), variant->userDataRoots.end(), userData.begin()) && (variant->registeredViews == nullptr || CaptureRegisteredViewsCurrent(*variant->registeredViews));
                if (!current) variant.reset();
                if (variant != nullptr) {
                    compared = 1;
                    current = true;
                    for (const auto& [begin, bytes] : variant->spans) {
                        GuestMemory::CollectWrites(begin, bytes);
                        if (!GuestMemory::UnchangedSince(begin, bytes, generation)) {
                            current = false;
                            break;
                        }
                    }
                }
                if (!current && variant != nullptr) {

                    std::uint64_t collected = 0;
                    for (const auto& [begin, bytes] : variant->spans) collected = std::max(collected, GuestMemory::CollectWrites(begin, bytes));
                    bool same = collected != 0;
                    if (same) {
                        const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DispatchCache);
                        PendingView pending;
                        pending.Load();
                        same = validateCaptured(address, submission.queue, variant->captured, *variant->compiled, false, pending);
                    }
                    if (same) {
                        restamped = collected;
                        current = true;
                    }
                }
                if (current) outcome = EntryOutcome::Equal;
            }
            phaseTiming.Phase(PhaseValidate);
            if (phaseTiming.profile) {
                const auto waited = std::min(Graphics::Recorder::ThreadWaitedMs() - waitedBeforeValidate, phaseMs[PhaseValidate]);
                phaseMs[PhaseValidate] -= waited;
                phaseMs[PhaseValidateWait] += waited;
            }
            if (!cacheLock.owns_lock()) cacheLock.lock();

            const auto again = dispatchCache.find(key);
            const bool untouched = again != dispatchCache.end() && again->second == entry && variants.front()->generation.load(std::memory_order_acquire) == generation;
            auto& counters = entryCounters;
            counters.validateUs += phaseMs[PhaseValidate] * 1000;
            counters.imagesFlushed += imagesFlushed;
            counters.runsSynced += runsSynced;
            for (std::size_t i = 0; i < compared && i < variants.size(); ++i) counters.runsValidated += variants[i]->runs.size();
            counters.retriesEqual += retriesEqual;
            counters.retriesMoved += retriesMoved;
            counters.variantsCompared += compared;
            switch (outcome) {
                case EntryOutcome::Equal: ++counters.equal; break;
                case EntryOutcome::EqualData: ++counters.equal; break;
                case EntryOutcome::Differing: ++counters.differing; break;
                case EntryOutcome::Inaccessible: ++counters.inaccessible; break;
                case EntryOutcome::QueuedLabel: ++counters.queuedLabel; break;
                case EntryOutcome::FlushingImage: ++counters.flushingImage; break;
                case EntryOutcome::PublishMoved: ++counters.publishMoved; break;
                case EntryOutcome::PendingMoved: ++counters.pendingMoved; break;
                case EntryOutcome::ForgetMoved: ++counters.forgetMoved; break;
            }
            if (current) {
                const auto& selected = effectiveVariant != nullptr ? effectiveVariant : variant;
                ++counters.variantHitsByRank[rank];
                compiledResult = selected->compiled;
                if (dataHit) {
                    const auto sourceWords = DispatchVariantWords(*selected);
                    liveWords.assign(sourceWords.begin(), sourceWords.end());
                    for (const auto& [position, value] : liveData) liveWords[position] = value;
                    regions.clear();
                    appendEntryRegions(*selected, regions, &liveWords);
                    compiledResult = patchDataWords(*selected, liveWords);
                    ++counters.dataHits;
                    counters.dataWordsRefreshed += liveData.size();
                    ++counters.dataHitsByRank[rank];
                }
                if (stampValidate()) {
                    captured = selected->captured;
                } else {
                    captured.reserve(memory.size() + regions.size());
                    captured.assign(memory.begin(), memory.end());
                    captured.insert(captured.end(), regions.begin(), regions.end());
                    if (selected->forgetSerial != GuestMemory::ForgetSerial()) ++counters.forgetSinceInsert;
                }
                keepVariant = selected;
                cached = true;
                ++dispatchCacheHits;
                if (untouched) {
                    if (effectiveVariant == nullptr && restamped != 0) variant->generation.store(restamped, std::memory_order_release);

                    if (rank != 0) {
                        auto rotated = std::make_shared<DispatchEntry>();
                        rotated->variants.reserve(variants.size());
                        rotated->variants.push_back(variant);
                        for (std::size_t i = 0; i < variants.size(); ++i) {
                            if (i != rank) rotated->variants.push_back(variants[i]);
                        }
                        rotated->touched = entry->touched;
                        rotated->order = entry->order;
                        again->second = std::move(rotated);
                    }

                    if (dispatchCacheHits - again->second->touched > dispatchCacheEntries() / 8) {
                        dispatchOrder.splice(dispatchOrder.begin(), dispatchOrder, again->second->order);
                        again->second->touched = dispatchCacheHits;
                        ++counters.touches;
                    }
                }
            } else {
                if (traceCache) std::fprintf(stderr, "[dispatch-cache] 0x%llx captured memory changed\n", static_cast<unsigned long long>(address));

                missedEntry = entry;
                missedDiffering = outcome == EntryOutcome::Differing;
                if (!untouched) ++counters.replaced;
            }
            if (profile && std::chrono::steady_clock::now() - counters.lastReport > std::chrono::seconds(10)) {
                counters.lastReport = std::chrono::steady_clock::now();
                reportDispatchCache(counters);
            }
            phaseTiming.Phase(PhaseRelock);
        }
    }
}

}
