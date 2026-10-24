#include "Theme.h"

#include <QApplication>
#include <QPalette>

void applyDarkTheme(QApplication &app)
{
    app.setStyle("Fusion");
    using R = QPalette::ColorRole;
    QPalette pal;
    const std::pair<R, QColor> roles[] = {
        {R::Window, {37, 37, 38}},          {R::WindowText, {220, 220, 220}},
        {R::Base, {30, 30, 30}},            {R::AlternateBase, {45, 45, 48}},
        {R::ToolTipBase, {50, 50, 55}},     {R::ToolTipText, {220, 220, 220}},
        {R::Text, {220, 220, 220}},         {R::PlaceholderText, {128, 128, 128}},
        {R::Button, {51, 51, 55}},          {R::ButtonText, {220, 220, 220}},
        {R::BrightText, {255, 80, 80}},     {R::Link, {86, 156, 214}},
        {R::Highlight, {38, 79, 120}},      {R::HighlightedText, {255, 255, 255}},
    };
    for (const auto &[role, col] : roles) pal.setColor(role, col);
    for (R role : {R::Text, R::WindowText, R::ButtonText}) pal.setColor(QPalette::Disabled, role, QColor(110, 110, 110));
    app.setPalette(pal);
}
