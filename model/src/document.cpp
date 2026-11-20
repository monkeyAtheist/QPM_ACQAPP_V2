#include "qacq/model/document.h"

#include <algorithm>
#include <unordered_set>

namespace qacq::model
{
namespace
{
bool samePointers(const std::vector<SignalPtr> &a, std::size_t aFrom, const std::vector<SignalPtr> &b, std::size_t bFrom,
             std::size_t count)
{
    for (std::size_t k = 0; k < count; ++k)
        if (a[aFrom + k] != b[bFrom + k]) return false;
    return true;
}
} // namespace

DocumentChange describeChange(const std::vector<SignalPtr> &before, const std::vector<SignalPtr> &after)
{
    DocumentChange change;
    const std::size_t n = before.size(), m = after.size();
    // First index where the lists differ (pointer comparison: same pointer = same signal, unchanged).
    std::size_t i = 0;
    while (i < n && i < m && before[i] == after[i]) ++i;
    if (n == m)
    {
        if (i == n) return change; // None
        // Same ids in the same order: updates.
        bool sameOrder = true;
        for (std::size_t k = 0; k < n && sameOrder; ++k) sameOrder = before[k]->id == after[k]->id;
        if (sameOrder)
        {
            change.kind = DocumentChange::Kind::Updated;
            for (std::size_t k = 0; k < n; ++k)
                if (before[k] != after[k]) change.rows.push_back(k);
            return change;
        }
        // One element moved: last differing index j.
        std::size_t j = n - 1;
        while (j > i && before[j] == after[j]) --j;
        if (before[i] == after[j] && samePointers(before, i + 1, after, i, j - i))
        {
            change.kind = DocumentChange::Kind::Moved; // down
            change.first = i;
            change.second = j;
            return change;
        }
        if (after[i] == before[j] && samePointers(before, i, after, i + 1, j - i))
        {
            change.kind = DocumentChange::Kind::Moved; // up
            change.first = j;
            change.second = i;
            return change;
        }
    }
    else if (m == n + 1 && samePointers(before, i, after, i + 1, n - i))
    {
        change.kind = DocumentChange::Kind::Inserted;
        change.first = i;
        return change;
    }
    else if (n == m + 1 && samePointers(before, i + 1, after, i, m - i))
    {
        change.kind = DocumentChange::Kind::Removed;
        change.first = i;
        return change;
    }
    change.kind = DocumentChange::Kind::Reset;
    return change;
}

Document::Document() = default;

SignalPtr Document::at(std::size_t index) const noexcept
{
    const auto &list = signalList();
    return index < list.size() ? list[index] : nullptr;
}

SignalPtr Document::find(std::uint64_t id) const noexcept
{
    for (const auto &s : signalList())
        if (s->id == id) return s;
    return nullptr;
}

std::optional<std::size_t> Document::indexOf(std::uint64_t id) const noexcept
{
    const auto &list = signalList();
    for (std::size_t i = 0; i < list.size(); ++i)
        if (list[i]->id == id) return i;
    return std::nullopt;
}

std::string Document::uniqueName(std::string_view base) const
{
    auto taken = [&](const std::string &name) {
        return std::any_of(signalList().begin(), signalList().end(), [&](const SignalPtr &s) { return s->name == name; });
    };
    std::string name(base);
    for (int k = 2; taken(name); ++k) name = std::string(base) + " " + std::to_string(k);
    return name;
}

std::uint64_t Document::add(Signal signal, std::string label, std::optional<std::size_t> index, std::string *error)
{
    const std::string problem = validateSignal(signal);
    if (!problem.empty())
    {
        if (error) *error = problem;
        return 0;
    }
    signal.id = m_nextId++;
    const std::uint64_t id = signal.id;
    std::vector<SignalPtr> state = signalList();
    const std::size_t at = std::min(index.value_or(state.size()), state.size());
    state.insert(state.begin() + static_cast<std::ptrdiff_t>(at), std::make_shared<const Signal>(std::move(signal)));
    commit(std::move(state), std::move(label), 0);
    return id;
}

bool Document::remove(std::uint64_t id, std::string *error)
{
    const auto index = indexOf(id);
    if (!index)
    {
        if (error) *error = "Ce signal n'existe plus.";
        return false;
    }
    std::vector<SignalPtr> state = signalList();
    const std::string label = "Supprimer « " + state[*index]->name + " »";
    state.erase(state.begin() + static_cast<std::ptrdiff_t>(*index));
    return commit(std::move(state), label, 0);
}

bool Document::replace(std::uint64_t id, Signal signal, std::string label, std::uint64_t mergeKey, std::string *error)
{
    const auto index = indexOf(id);
    if (!index)
    {
        if (error) *error = "Ce signal n'existe plus.";
        return false;
    }
    signal.id = id;
    const std::string problem = validateSignal(signal);
    if (!problem.empty())
    {
        if (error) *error = problem;
        return false;
    }
    std::vector<SignalPtr> state = signalList();
    state[*index] = std::make_shared<const Signal>(std::move(signal));
    return commit(std::move(state), std::move(label), mergeKey);
}

bool Document::rename(std::uint64_t id, std::string name, std::string *error)
{
    const SignalPtr current = find(id);
    if (!current)
    {
        if (error) *error = "Ce signal n'existe plus.";
        return false;
    }
    if (name.empty())
    {
        if (error) *error = "Le nom ne peut pas être vide.";
        return false;
    }
    if (name == current->name) return true;
    Signal copy = *current; // the samples stay shared
    const std::string label = "Renommer « " + current->name + " »";
    copy.name = std::move(name);
    return replace(id, std::move(copy), label, 0, error);
}

bool Document::move(std::size_t from, std::size_t to)
{
    std::vector<SignalPtr> state = signalList();
    if (from >= state.size() || to >= state.size()) return false;
    if (from == to) return true;
    SignalPtr item = state[from];
    state.erase(state.begin() + static_cast<std::ptrdiff_t>(from));
    state.insert(state.begin() + static_cast<std::ptrdiff_t>(to), item);
    return commit(std::move(state), "Déplacer « " + item->name + " »", 0);
}

std::string Document::undoText() const
{
    return canUndo() ? m_steps[m_current].label : std::string();
}

std::string Document::redoText() const
{
    return canRedo() ? m_steps[m_current + 1].label : std::string();
}

bool Document::undo()
{
    if (!canUndo()) return false;
    const std::vector<SignalPtr> before = signalList();
    --m_current;
    m_mergeOpen = false;
    notify(before);
    return true;
}

bool Document::redo()
{
    if (!canRedo()) return false;
    const std::vector<SignalPtr> before = signalList();
    ++m_current;
    m_mergeOpen = false;
    notify(before);
    return true;
}

bool Document::commit(std::vector<SignalPtr> state, std::string label, std::uint64_t mergeKey)
{
    const std::vector<SignalPtr> before = signalList();
    // A new edit drops the steps that could have been redone.
    if (m_current + 1 < m_steps.size())
    {
        m_steps.erase(m_steps.begin() + static_cast<std::ptrdiff_t>(m_current + 1), m_steps.end());
        if (m_clean && *m_clean > m_current) m_clean.reset();
    }
    const bool merge = mergeKey != 0 && m_mergeOpen && m_current > 0 && m_steps[m_current].mergeKey == mergeKey;
    if (merge)
    {
        m_steps[m_current].state = std::move(state);
        if (m_clean && *m_clean == m_current) m_clean.reset();
    }
    else
    {
        m_steps.push_back(Step{std::move(state), std::move(label), mergeKey});
        ++m_current;
    }
    m_mergeOpen = mergeKey != 0;
    trim();
    notify(before);
    return true;
}

std::size_t Document::historyBytes() const
{
    std::unordered_set<const SampleData *> live, held;
    for (const auto &s : signalList())
        if (s->data) live.insert(s->data.get());
    std::size_t bytes = 0;
    for (std::size_t k = 0; k < m_steps.size(); ++k)
    {
        if (k == m_current) continue;
        for (const auto &s : m_steps[k].state)
        {
            const SampleData *d = s->data.get();
            if (d && !live.count(d) && held.insert(d).second) bytes += d->byteSize();
        }
    }
    return bytes;
}

void Document::setHistoryLimits(std::size_t bytes, std::size_t steps)
{
    m_budgetBytes = bytes;
    m_budgetSteps = std::max<std::size_t>(1, steps);
    trim();
}

void Document::trim()
{
    // Drop the oldest steps; the current state is never dropped.
    while (m_current > 0 && (m_steps.size() - 1 > m_budgetSteps || historyBytes() > m_budgetBytes))
    {
        m_steps.erase(m_steps.begin());
        --m_current;
        if (m_clean)
        {
            if (*m_clean == 0) m_clean.reset();
            else --*m_clean;
        }
    }
    // Beyond the budget even with no step to undo: drop the redo steps, newest first.
    while (m_current + 1 < m_steps.size() && historyBytes() > m_budgetBytes)
    {
        m_steps.pop_back();
        if (m_clean && *m_clean >= m_steps.size()) m_clean.reset();
    }
}

void Document::reset(std::vector<SignalPtr> list)
{
    const std::vector<SignalPtr> before = signalList();
    list.erase(std::remove(list.begin(), list.end(), nullptr), list.end());
    std::uint64_t largest = 0;
    for (const auto &s : list)
        if (s) largest = std::max(largest, s->id);
    m_nextId = largest + 1;
    std::unordered_set<std::uint64_t> seen;
    for (auto &s : list)
    {
        if (s->id == 0 || !seen.insert(s->id).second)
        {
            auto copy = std::make_shared<Signal>(*s);
            copy->id = m_nextId++;
            seen.insert(copy->id);
            s = std::move(copy);
        }
    }
    m_steps.assign(1, Step{std::move(list), {}, 0});
    m_current = 0;
    m_clean = 0;
    m_mergeOpen = false;
    notify(before);
}

void Document::notify(const std::vector<SignalPtr> &before)
{
    if (!m_listener) return;
    const DocumentChange change = describeChange(before, signalList());
    if (change.kind != DocumentChange::Kind::None) m_listener(change);
}
} // namespace qacq::model
