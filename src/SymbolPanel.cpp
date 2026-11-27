#include "SymbolPanel.h"

#include <QGridLayout>
#include <QScrollArea>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

const QVector<SymbolPanel::Group> &SymbolPanel::groups()
{
    static const QVector<Group> g = {
        {"Greek", {
            {"α", "\\alpha"}, {"β", "\\beta"}, {"γ", "\\gamma"}, {"δ", "\\delta"}, {"ε", "\\epsilon"},
            {"ϵ", "\\varepsilon"}, {"ζ", "\\zeta"}, {"η", "\\eta"}, {"θ", "\\theta"}, {"ϑ", "\\vartheta"},
            {"ι", "\\iota"}, {"κ", "\\kappa"}, {"λ", "\\lambda"}, {"μ", "\\mu"}, {"ν", "\\nu"},
            {"ξ", "\\xi"}, {"π", "\\pi"}, {"ρ", "\\rho"}, {"σ", "\\sigma"}, {"τ", "\\tau"},
            {"υ", "\\upsilon"}, {"φ", "\\phi"}, {"ϕ", "\\varphi"}, {"χ", "\\chi"}, {"ψ", "\\psi"},
            {"ω", "\\omega"}, {"Γ", "\\Gamma"}, {"Δ", "\\Delta"}, {"Θ", "\\Theta"}, {"Λ", "\\Lambda"},
            {"Ξ", "\\Xi"}, {"Π", "\\Pi"}, {"Σ", "\\Sigma"}, {"Υ", "\\Upsilon"}, {"Φ", "\\Phi"},
            {"Ψ", "\\Psi"}, {"Ω", "\\Omega"}}},
        {"Operators", {
            {"∑", "\\sum_{%|}^{}"}, {"∏", "\\prod_{%|}^{}"}, {"∫", "\\int_{%|}^{}"}, {"∮", "\\oint"},
            {"∬", "\\iint"}, {"a⁄b", "\\frac{%|}{}"}, {"√", "\\sqrt{%|}"}, {"∂", "\\partial"},
            {"∇", "\\nabla"}, {"∞", "\\infty"}, {"lim", "\\lim_{%|}"}, {"±", "\\pm"}, {"∓", "\\mp"},
            {"×", "\\times"}, {"·", "\\cdot"}, {"÷", "\\div"}, {"⊗", "\\otimes"}, {"⊕", "\\oplus"},
            {"∘", "\\circ"}, {"∪", "\\cup"}, {"∩", "\\cap"}, {"x̂", "\\hat{%|}"}, {"x̄", "\\bar{%|}"},
            {"x⃗", "\\vec{%|}"}, {"ẋ", "\\dot{%|}"}, {"x̃", "\\tilde{%|}"}, {"( )", "\\left( %| \\right)"},
            {"[ ]", "\\left[ %| \\right]"}, {"⟨ ⟩", "\\langle %| \\rangle"}, {"| |", "\\left| %| \\right|"}}},
        {"Relations", {
            {"≤", "\\leq"}, {"≥", "\\geq"}, {"≠", "\\neq"}, {"≈", "\\approx"}, {"≡", "\\equiv"},
            {"∼", "\\sim"}, {"≃", "\\simeq"}, {"∝", "\\propto"}, {"≪", "\\ll"}, {"≫", "\\gg"},
            {"∈", "\\in"}, {"∉", "\\notin"}, {"⊂", "\\subset"}, {"⊆", "\\subseteq"}, {"⊃", "\\supset"},
            {"∀", "\\forall"}, {"∃", "\\exists"}, {"∅", "\\emptyset"}, {"⊥", "\\perp"}, {"∥", "\\parallel"}}},
        {"Arrows", {
            {"→", "\\to"}, {"←", "\\leftarrow"}, {"↔", "\\leftrightarrow"}, {"⇒", "\\Rightarrow"},
            {"⇐", "\\Leftarrow"}, {"⇔", "\\Leftrightarrow"}, {"↦", "\\mapsto"}, {"↑", "\\uparrow"},
            {"↓", "\\downarrow"}, {"⟶", "\\longrightarrow"}, {"⇌", "\\rightleftharpoons"}}},
        {"Physics", {
            {"ℏ", "\\hbar"}, {"†", "^\\dagger"}, {"⟨|", "\\langle %| |"}, {"|⟩", "| %| \\rangle"},
            {"⟨|⟩", "\\langle %| | \\rangle"}, {"ℓ", "\\ell"}, {"Å", "\\AA"}, {"°", "^\\circ"},
            {"γ*", "\\gamma^*"}, {"q̄", "\\bar{q}"}, {"√s", "\\sqrt{s_{NN}}"}, {"p_T", "p_{\\mathrm{T}}"},
            {"d/dx", "\\frac{d %|}{dx}"}, {"∂/∂x", "\\frac{\\partial %|}{\\partial x}"},
            {"e", "\\mathrm{e}"}, {"GeV", "\\,\\mathrm{GeV}"}}},
    };
    return g;
}

SymbolPanel::SymbolPanel(QWidget *parent) : QWidget(parent)
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    auto *tabs = new QTabWidget(this);
    tabs->setDocumentMode(true);
    lay->addWidget(tabs);

    for (const Group &g : groups()) {
        auto *page = new QWidget;
        auto *grid = new QGridLayout(page);
        grid->setSpacing(2);
        const int cols = 6;
        int k = 0;
        for (const Symbol &s : g.symbols) {
            auto *b = new QToolButton(page);
            b->setText(QString::fromUtf8(s.glyph));
            const QString latex = QString::fromUtf8(s.latex);
            QString tip = latex;
            b->setToolTip(tip.remove("%|"));
            b->setMinimumSize(34, 30);
            b->setAutoRaise(true);
            connect(b, &QToolButton::clicked, this, [this, latex]() { emit insertRequested(latex); });
            grid->addWidget(b, k / cols, k % cols);
            ++k;
        }
        grid->setRowStretch(k / cols + 1, 1);
        auto *scroll = new QScrollArea;
        scroll->setWidget(page);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        tabs->addTab(scroll, tr(g.name));
    }
}
