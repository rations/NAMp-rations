// RackModel implementation. See rackmodel.h.

#include "rackmodel.h"

#include <algorithm>

namespace NAMp::rack
{

//------------------------------------------------------------------------
void RackModel::refresh()
{
    mNodes.clear();
    mTotalLatency = 0;
    if (!mBuilder)
        return;

    for (const auto s : {host::ChainSection::Pre, host::ChainSection::Post}) {
        const int count = mBuilder->count(s);
        for (int i = 0; i < count; ++i) {
            host::ChainNodeInfo info;
            if (!mBuilder->nodeInfo(s, i, info))
                continue;

            RackNode node;
            node.id = info.id;
            node.section = s;
            node.index = i;
            node.name = info.name;
            node.enabled = info.enabled;
            node.mix = info.mix;
            node.latency = info.latency;
            node.placeholder = info.placeholder;

            if (host::PluginBackend *backend = mBuilder->backend(s, i)) {
                node.hasEditor = backend->editorKind() != host::EditorKind::NoEditor;
                if (mDiagArmed) {
                    node.diagAllocs = backend->diagRtAllocCount();
                    node.diagMicros = latchDiagPeak(node.id, backend->diagTakeMaxMicros());
                }
            }
            node.editorOpen =
                std::find(mEditorsOpen.begin(), mEditorsOpen.end(), node.id) != mEditorsOpen.end();

            mNodes.push_back(std::move(node));
        }
    }

    mTotalLatency = mBuilder->latencySamples();
}

//------------------------------------------------------------------------
int64_t RackModel::latchDiagPeak(uint64_t id, int64_t micros)
{
    for (DiagPeak &peak : mDiagPeaks) {
        if (peak.id != id)
            continue;
        if (micros > 0)
            peak.micros = micros;
        return peak.micros;
    }
    // Ids are never reused, so this list only grows with nodes that have existed this session —
    // bounded by how many pedals the user has loaded, not by time.
    mDiagPeaks.push_back(DiagPeak{id, micros});
    return micros;
}

//------------------------------------------------------------------------
int64_t RackModel::diagWorstMicros() const
{
    int64_t worst = 0;
    for (const RackNode &node : mNodes)
        worst = std::max(worst, node.diagMicros);
    return worst;
}

//------------------------------------------------------------------------
std::vector<const RackNode *> RackModel::section(host::ChainSection s, bool routedOnly) const
{
    std::vector<const RackNode *> out;
    for (const RackNode &node : mNodes) {
        if (node.section != s)
            continue;
        if (routedOnly && !node.enabled)
            continue;
        out.push_back(&node);
    }
    return out;
}

//------------------------------------------------------------------------
const RackNode *RackModel::nodeById(uint64_t id) const
{
    for (const RackNode &node : mNodes) {
        if (node.id == id)
            return &node;
    }
    return nullptr;
}

//------------------------------------------------------------------------
// Routed slots count only enabled nodes, but ChainBuilder::move works on the full list, so the two
// have to be translated between. Slot k lands immediately before the k-th routed node; slot n (one
// past the last) lands at the end of the whole section, disabled nodes included, which is where a
// node dropped at the end of the path belongs.
int RackModel::fullIndexForRoutedSlot(host::ChainSection s, int slot) const
{
    if (slot <= 0)
        return 0;

    int seen = 0;
    for (const RackNode &node : mNodes) {
        if (node.section != s || !node.enabled)
            continue;
        if (seen == slot)
            return node.index;
        ++seen;
    }

    int last = 0;
    for (const RackNode &node : mNodes) {
        if (node.section == s)
            last = node.index + 1;
    }
    return last;
}

//------------------------------------------------------------------------
int RackModel::catalogCount(host::PluginFormat format) const
{
    if (!mCatalog)
        return 0;
    int count = 0;
    for (const host::PluginDesc &desc : *mCatalog) {
        if (desc.ref.format == format)
            ++count;
    }
    return count;
}

//------------------------------------------------------------------------
int RackModel::freshCount() const
{
    if (!mCatalog)
        return 0;
    int count = 0;
    for (const host::PluginDesc &desc : *mCatalog) {
        if (desc.freshlyFound)
            ++count;
    }
    return count;
}

//------------------------------------------------------------------------
void RackModel::setEditorOpen(uint64_t id, bool open)
{
    if (id == 0)
        return;
    const auto it = std::find(mEditorsOpen.begin(), mEditorsOpen.end(), id);
    if (open && it == mEditorsOpen.end())
        mEditorsOpen.push_back(id);
    else if (!open && it != mEditorsOpen.end())
        mEditorsOpen.erase(it);
}

} // namespace NAMp::rack
