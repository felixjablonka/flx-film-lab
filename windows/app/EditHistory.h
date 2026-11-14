#pragma once

#include <QGuiApplication>
#include <QObject>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Undo and redo (Ctrl+Z, Ctrl+Y or Ctrl+Shift+Z) for the edits of the
// picture or the clip on screen: the Mac app's EditHistory.
//
// A step is one gesture, however many values it passes through: a slider
// dragged from press to release, a handle, a crop edge or a curve point
// dragged, a click on a switch, a choice from a menu, a reset or a paste.
// While the mouse button is down, every change joins the step that is open,
// so a drag is one step; a change made without it joins only those made by the
// same action. A quick repeat of the same change joins a step made with the
// button down: the second click of the double-click that resets a slider.
// Each step holds the edits as they were before it and after it, so undoing
// one puts back exactly what it started from.
template<typename State>
class EditHistory
{
public:
    struct Step {
        std::string name;
        State before, after;
        uint64_t id = 0;
    };
    // The steps that can be undone, oldest first, and the ones undone since,
    // the most recently undone last.
    struct Stacks {
        std::vector<Step> undo, redo;
    };
    struct Mark {
        size_t count = 0;
        uint64_t top = 0;
        std::vector<Step> redo;
    };

    // `read` gives the edits as they stand, `write` puts a set back,
    // `describe` names the change from one set to another, and `changed` is
    // told when what Undo and Redo would do has changed.
    EditHistory(QObject *context, std::function<State()> read, std::function<void(const State &)> write,
                std::function<std::string(const State &, const State &)> describe, std::function<void()> changed)
        : m_context(context), m_read(std::move(read)), m_write(std::move(write)), m_describe(std::move(describe)),
          m_changed(std::move(changed))
    {
    }

    const Stacks &stacks() const { return m_stacks; }
    // Whether a step is open, waiting for its action or gesture to end.
    bool isStepOpen() const { return m_open.has_value(); }

    // `stacks` with a step added on top, as if it had been taken while they
    // were on screen: a picture's share of Paste Settings to Selected, kept in
    // its own history while another picture is on screen.
    static Stacks adding(const std::string &name, const State &before, const State &after, Stacks stacks)
    {
        stacks.undo.push_back(Step{name, before, after, ++s_nextID});
        if (stacks.undo.size() > limit)
            stacks.undo.erase(stacks.undo.begin(), stacks.undo.end() - limit);
        stacks.redo.clear();
        return stacks;
    }
    std::optional<std::string> undoName() const
    {
        return m_stacks.undo.empty() ? std::nullopt : std::optional(m_stacks.undo.back().name);
    }
    std::optional<std::string> redoName() const
    {
        return m_stacks.redo.empty() ? std::nullopt : std::optional(m_stacks.redo.back().name);
    }

    // Whether the mouse button is down: while it is, a step stays open.
    std::function<bool()> mouseIsDown = [] { return (QGuiApplication::mouseButtons() & Qt::LeftButton) != 0; };
    // How soon a repeat of the same change has to start to join the step
    // before it; the app sets it to the system's double-click interval.
    double joinSeconds = 0.5;
    static constexpr size_t limit = 200;

    // The edits are about to change: `before` gives them as they are. The
    // first change opens a step; it closes once the action that made it is
    // over, or, while the mouse button is down, once the button comes up.
    void note(const std::function<State()> &before)
    {
        if (!m_recording || m_open)
            return;
        m_open = Open{before(), m_nextName, now(), mouseIsDown()};
        const uint64_t generation = ++m_generation;
        // after the action that made the change has finished
        QTimer::singleShot(0, m_context, [this, generation] { poll(generation, false); });
    }

    // Close the open step, keeping it if it changed anything.
    void finish()
    {
        ++m_generation;
        if (!m_open)
            return;
        const Open open = std::move(*m_open);
        m_open.reset();
        const State after = m_read();
        if (after == open.before)
            return;
        const std::string name = open.name ? *open.name : m_describe(open.before, after);
        bool joined = false;
        if (!open.name && !m_stacks.undo.empty() && m_joinable && m_joinable->first == m_stacks.undo.back().id
            && open.at < m_joinable->second && m_stacks.undo.back().name == name
            && m_stacks.undo.back().after == open.before) {
            // a quick repeat of the same change belongs to the step before it,
            // and one that undoes it leaves nothing to undo
            Step last = std::move(m_stacks.undo.back());
            m_stacks.undo.pop_back();
            if (!(last.before == after))
                m_stacks.undo.push_back(Step{name, std::move(last.before), after, last.id});
            joined = true;
        } else {
            m_stacks.undo.push_back(Step{name, open.before, after, ++s_nextID});
            if (m_stacks.undo.size() > limit)
                m_stacks.undo.erase(m_stacks.undo.begin(), m_stacks.undo.end() - limit);
        }
        // a named step (a reset, a paste) stands alone, and so does one made
        // by a key or a click on a button
        if (!open.name && (open.pressed || joined) && !m_stacks.undo.empty())
            m_joinable = std::pair(m_stacks.undo.back().id, now() + joinSeconds);
        else
            m_joinable.reset();
        m_stacks.redo.clear();
        m_changed();
    }

    void undo()
    {
        finish();
        if (m_stacks.undo.empty())
            return;
        Step step = std::move(m_stacks.undo.back());
        m_stacks.undo.pop_back();
        const State before = step.before;
        m_stacks.redo.push_back(std::move(step));
        m_joinable.reset();
        unrecorded([&] { m_write(before); });
        m_changed();
    }

    void redo()
    {
        finish();
        if (m_stacks.redo.empty())
            return;
        Step step = std::move(m_stacks.redo.back());
        m_stacks.redo.pop_back();
        const State after = step.after;
        m_stacks.undo.push_back(std::move(step));
        m_joinable.reset();
        unrecorded([&] { m_write(after); });
        m_changed();
    }

    // Whatever `change` changes as one step of its own, called `name`:
    // "Reset Tone", "Paste Settings".
    void named(const std::string &name, const std::function<void()> &change)
    {
        finish();
        m_nextName = name;
        change();
        m_nextName.reset();
        finish();
    }

    // Change the edits without a step: another picture's coming on screen.
    void unrecorded(const std::function<void()> &change)
    {
        const bool was = m_recording;
        m_recording = false;
        change();
        m_recording = was;
    }

    // The steps of the picture or clip coming on screen, for those of the one
    // going, which are given back. A step still open is the one going's.
    Stacks swap(Stacks incoming)
    {
        finish();
        Stacks outgoing = std::move(m_stacks);
        m_stacks = std::move(incoming);
        m_joinable.reset();
        m_changed();
        return outgoing;
    }

    Mark mark()
    {
        finish();
        return Mark{m_stacks.undo.size(), m_stacks.undo.empty() ? 0 : m_stacks.undo.back().id, m_stacks.redo};
    }

    // Take the steps since `mark` off as if they had never been taken, and let
    // what could be redone then be redone again: when every one of them
    // changed only what `allowed` allows. Putting the edits back is the
    // caller's. False, with nothing changed, when a step since changed
    // something else, or the history was undone past `mark`.
    bool rewind(const Mark &mark, const std::function<bool(const State &, const State &)> &allowed)
    {
        finish();
        auto &undo = m_stacks.undo;
        if (undo.size() < mark.count || (mark.count > 0 && undo[mark.count - 1].id != mark.top))
            return false;
        for (size_t i = mark.count; i < undo.size(); ++i)
            if (!allowed(undo[i].before, undo[i].after))
                return false;
        undo.resize(mark.count);
        m_stacks.redo = mark.redo;
        m_joinable.reset();
        m_changed();
        return true;
    }

private:
    struct Open {
        State before;
        std::optional<std::string> name;
        double at = 0;
        bool pressed = false;
    };

    static double now()
    {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    // While the button is down the step stays open; once it is up, what the
    // button coming up set (the reset at the end of a double-click) gets 50 ms
    // to join it.
    void poll(uint64_t generation, bool released)
    {
        if (generation != m_generation || !m_open)
            return;
        if (mouseIsDown()) {
            QTimer::singleShot(30, m_context, [this, generation] { poll(generation, true); });
        } else if (released) {
            QTimer::singleShot(50, m_context, [this, generation] {
                if (generation != m_generation)
                    return;
                if (mouseIsDown())
                    poll(generation, true);
                else
                    finish();
            });
        } else {
            finish();
        }
    }

    QObject *m_context;
    std::function<State()> m_read;
    std::function<void(const State &)> m_write;
    std::function<std::string(const State &, const State &)> m_describe;
    std::function<void()> m_changed;
    Stacks m_stacks;
    std::optional<Open> m_open;
    std::optional<std::string> m_nextName;
    std::optional<std::pair<uint64_t, double>> m_joinable;
    bool m_recording = true;
    uint64_t m_generation = 0;
    // shared by every history, so a step added to one put away (adding) is
    // never taken for another's
    static inline uint64_t s_nextID = 0;
};
