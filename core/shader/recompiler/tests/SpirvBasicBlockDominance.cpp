#include "source/val/basic_block.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {

using spvtools::val::BasicBlock;

using Relation = bool (BasicBlock::*)(const BasicBlock&) const;
using Begin = const BasicBlock::DominatorIterator (BasicBlock::*)() const;
using End = const BasicBlock::DominatorIterator (BasicBlock::*)() const;

bool Reference(const BasicBlock& block, const BasicBlock& other, Begin begin, End end) {
    return (&block == &other) ||
           !((other.*end)() == std::find((other.*begin)(), (other.*end)(), &block));
}

bool Check(const std::vector<std::unique_ptr<BasicBlock>>& blocks, Relation relation,
           Begin begin, End end) {
    for (const auto& block : blocks) {
        for (const auto& other : blocks) {
            if ((block.get()->*relation)(*other) != Reference(*block, *other, begin, end)) {
                return false;
            }
        }
    }
    return true;
}

}

int main() {
    std::vector<std::unique_ptr<BasicBlock>> blocks;
    for (uint32_t id = 0; id < 40; ++id) {
        blocks.push_back(std::make_unique<BasicBlock>(id));
    }
    for (uint32_t id = 1; id < 20; ++id) {
        blocks[id]->SetImmediateDominator(blocks[id - 1].get());
        blocks[id]->SetImmediateStructuralDominator(blocks[id / 2].get());
        blocks[id]->SetImmediateStructuralPostDominator(id == 19 ? nullptr : blocks[id + 1].get());
    }
    for (uint32_t id : {20, 21}) {
        blocks[id]->SetImmediateDominator(blocks[id].get());
        blocks[id]->SetImmediateStructuralDominator(blocks[id].get());
        blocks[id]->SetImmediateStructuralPostDominator(blocks[id].get());
    }
    blocks[22]->SetImmediateDominator(blocks[20].get());
    blocks[22]->SetImmediateStructuralDominator(blocks[20].get());
    blocks[22]->SetImmediateStructuralPostDominator(blocks[20].get());
    blocks[23]->SetImmediateDominator(blocks[21].get());
    blocks[23]->SetImmediateStructuralDominator(blocks[21].get());
    blocks[23]->SetImmediateStructuralPostDominator(blocks[21].get());
    blocks[24]->SetImmediateDominator(blocks[20].get());
    blocks[24]->SetImmediateStructuralDominator(blocks[20].get());
    blocks[24]->SetImmediateStructuralPostDominator(blocks[20].get());
    for (uint32_t id : {25, 26}) {
        blocks[id]->SetImmediateDominator(blocks[22].get());
        blocks[id]->SetImmediateStructuralDominator(blocks[22].get());
        blocks[id]->SetImmediateStructuralPostDominator(blocks[22].get());
    }
    blocks[27]->SetImmediateDominator(nullptr);
    blocks[27]->SetImmediateStructuralDominator(nullptr);
    blocks[27]->SetImmediateStructuralPostDominator(nullptr);
    if (!Check(blocks, &BasicBlock::dominates, &BasicBlock::dom_begin, &BasicBlock::dom_end) ||
        !Check(blocks, &BasicBlock::structurally_dominates, &BasicBlock::structural_dom_begin,
               &BasicBlock::structural_dom_end) ||
        !Check(blocks, &BasicBlock::structurally_postdominates, &BasicBlock::structural_pdom_begin,
               &BasicBlock::structural_pdom_end)) {
        std::fprintf(stderr, "dominance traversal differs from iterator semantics\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
