#pragma once
// Read-only navigation: never invokes a tuning control or hardware scanner.
class QTabWidget;
class QWidget;
namespace uinavigator {
/// Search available tabs, Health catalogue entries and visible controls (Ctrl+K).
void open(QTabWidget *tabs, QWidget *parent);
}
