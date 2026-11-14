#pragma once
#include <cstddef>
#include <cstdint>

namespace novabar
{
    class CaptureScheduler
    {
        size_t m_visibleCursor = 0, m_hiddenCursor = 0;
        double m_lastHiddenWork = 0, m_lastWarm = 0;
        static bool BudgetLeft(double now, double start) { return now >= start && now - start < 16; }
    public:
        template<class Now, class Visible, class HiddenDue, class Update>
        void Run(size_t count, double budgetStart, Now now, Visible isVisible, HiddenDue isHiddenDue, Update update)
        {
            if (!count) return;
            const size_t firstVisible = m_visibleCursor % count;
            size_t index = firstVisible;
            for (size_t scanned = 0; scanned < count; ++scanned)
            {
                if (isVisible(index))
                {
                    update(index);
                    m_visibleCursor = index + 1 == count ? 0 : index + 1;
                    // Discovery may already have spent the budget. Always give
                    // one eligible visible target a complete update first.
                    if (!BudgetLeft(now(), budgetStart)) break;
                }
                index = index + 1 == count ? 0 : index + 1;
            }
            const double current = now();
            if (!BudgetLeft(current, budgetStart)
                && (current < m_lastHiddenWork || current - m_lastHiddenWork < 32)) return;
            index = m_hiddenCursor % count;
            for (size_t scanned = 0; scanned < count; ++scanned)
            {
                if (!isVisible(index) && isHiddenDue(index))
                {
                    update(index);
                    m_hiddenCursor = index + 1 == count ? 0 : index + 1;
                    m_lastHiddenWork = now();
                    break;
                }
                index = index + 1 == count ? 0 : index + 1;
            }
        }
        bool ShouldWarm(bool visiblePanel, double now, double budgetStart) const
        {
            return !visiblePanel && BudgetLeft(now, budgetStart) && now >= m_lastWarm && now - m_lastWarm >= 250;
        }
        void DidWarm(double now) { m_lastWarm = now; }
    };
}
