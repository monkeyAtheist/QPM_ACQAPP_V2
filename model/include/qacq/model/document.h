#pragma once

// The signals of a session, with undo and redo.
//
// The history keeps whole states (the ordered list of shared signal pointers): undo is exact whatever the
// edit, and cheap because unchanged signals and samples are shared between states. What the history
// costs is the samples it alone keeps alive (a removed or replaced signal): the oldest steps are dropped
// past a memory budget (1 GiB by default) or a number of steps (200).
//
// Every change (edit, undo, redo, reset) is reported to the listener with a description the Qt list model
// turns into row insertions, removals, moves or updates.

#include "qacq/model/signal.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qacq::model
{
struct DocumentChange
{
    enum class Kind
    {
        None,
        Inserted, // row `first` of the new list
        Removed,  // row `first` of the old list
        Moved,    // from row `first` to row `second` (index in the new list)
        Updated,  // rows `rows`, same order and same ids
        Reset     // anything else
    };
    Kind kind = Kind::None;
    std::size_t first = 0;
    std::size_t second = 0;
    std::vector<std::size_t> rows;
};

DocumentChange describeChange(const std::vector<SignalPtr> &before, const std::vector<SignalPtr> &after);

class Document
{
public:
    using Listener = std::function<void(const DocumentChange &)>;

    static constexpr std::size_t kDefaultHistoryBytes = std::size_t{1} << 30;
    static constexpr std::size_t kDefaultHistorySteps = 200;

    Document();

    // Read access.
    const std::vector<SignalPtr> &signalList() const noexcept { return m_steps[m_current].state; }
    std::size_t count() const noexcept { return signalList().size(); }
    SignalPtr at(std::size_t index) const noexcept;
    SignalPtr find(std::uint64_t id) const noexcept;
    std::optional<std::size_t> indexOf(std::uint64_t id) const noexcept;
    // "base", or "base 2", "base 3"... if a signal already has that name.
    std::string uniqueName(std::string_view base) const;

    // Edits: one undo step each, named by `label` ("Annuler : <label>"). On failure nothing changes and
    // *error (if given) says why, in French.
    // add: the document gives the id; at the end of the list unless an index is given. Returns the id, 0 on failure.
    std::uint64_t add(Signal signal, std::string label = "Ajouter un signal", std::optional<std::size_t> index = {},
                      std::string *error = nullptr);
    bool remove(std::uint64_t id, std::string *error = nullptr);
    // Replaces the signal (same id, same place). Consecutive calls with the same non-zero mergeKey make a
    // single undo step (a slider being dragged) until endMerge().
    bool replace(std::uint64_t id, Signal signal, std::string label, std::uint64_t mergeKey = 0,
                 std::string *error = nullptr);
    bool rename(std::uint64_t id, std::string name, std::string *error = nullptr);
    bool move(std::size_t from, std::size_t to);
    void endMerge() noexcept { m_mergeOpen = false; }

    // History.
    bool canUndo() const noexcept { return m_current > 0; }
    bool canRedo() const noexcept { return m_current + 1 < m_steps.size(); }
    std::string undoText() const;
    std::string redoText() const;
    bool undo();
    bool redo();
    std::size_t historySteps() const noexcept { return m_steps.size() - 1; }
    // Bytes of samples that only the history keeps alive.
    std::size_t historyBytes() const;
    void setHistoryLimits(std::size_t bytes, std::size_t steps);

    // Modified since the last save (or since the reset).
    bool isModified() const noexcept { return !m_clean || *m_clean != m_current; }
    void setClean() noexcept { m_clean = m_current; }

    // New session or loaded session: replaces the list, empties the history, unmodified. The ids are kept
    // (missing ones are given) and the next ids follow the largest.
    void reset(std::vector<SignalPtr> list = {});

    void setListener(Listener listener) { m_listener = std::move(listener); }

private:
    struct Step
    {
        std::vector<SignalPtr> state;
        std::string label;
        std::uint64_t mergeKey = 0;
    };

    bool commit(std::vector<SignalPtr> state, std::string label, std::uint64_t mergeKey);
    void trim();
    void notify(const std::vector<SignalPtr> &before);

    std::vector<Step> m_steps{Step{}};
    std::size_t m_current = 0;
    std::optional<std::size_t> m_clean{0};
    bool m_mergeOpen = false;
    std::uint64_t m_nextId = 1;
    std::size_t m_budgetBytes = kDefaultHistoryBytes;
    std::size_t m_budgetSteps = kDefaultHistorySteps;
    Listener m_listener;
};
} // namespace qacq::model
