#pragma once
#include <QMainWindow>

class AmdGpuTab;
class FwattrTab;
class HealthTab;
class HomeTab;
class LauncherWatch;
class DriftWatch;
class LidWatch;
class SmartWatch;
class SceneEngine;
class GlobalShortcuts;
class IntelTab;
class LightingTab;
class NvidiaTab;
class OptimizeTab;
class RyzenTab;
class QTabWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    HomeTab *home() const { return home_; }
    RyzenTab *ryzen() const { return ryzen_; }  // nullptr on non-AMD CPUs
    IntelTab *intel() const { return intel_; }  // nullptr on non-Intel CPUs
    NvidiaTab *nvidia() const { return nvidia_; }
    AmdGpuTab *amdgpu() const { return amdgpu_; }  // nullptr without an amdgpu card
    OptimizeTab *optimize() const { return optimize_; }
    FwattrTab *fwattr() const { return fwattr_; }
    LightingTab *lighting() const { return lighting_; }  // nullptr without a Spectrum keyboard
    SceneEngine *scenes() const { return scenes_; }
    GlobalShortcuts *shortcuts() const { return shortcuts_; }
    HealthTab *health() const { return health_; }
    LauncherWatch *launchers() const { return launchers_; }
    DriftWatch *drift() const { return drift_; }
    SmartWatch *smart() const { return smart_; }
    LidWatch *lid() const { return lid_; }
    /// Creates the NVIDIA tab if the dGPU has appeared since start-up (after
    /// "Bring NVIDIA back" / leaving iGPU-only). Returns the tab, or nullptr.
    NvidiaTab *ensureNvidiaTab();
    /// Set by the tray's Quit: closeEvent then really closes instead of hiding.
    void setForceQuit(bool v) { forceQuit_ = v; }
    void setHideOnClose(bool v) { hideOnClose_ = v; }
    /// Shows (and raises) the window on the tab with this label.
    void showTab(const QString &label);
    /// Rewrites the MangoHud overlay file (active scene · power profile), or removes it when the overlay is off.
    void updateOverlay();

Q_SIGNALS:
    void nvidiaTabAdded();

protected:
    void closeEvent(QCloseEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    void fitTabs();
    QTabWidget *tabs_;
    int tabW_ = -1;
    QWidget *mark_;  // brand: icon + wordmark, floated over the header bezel
    HomeTab *home_;
    RyzenTab *ryzen_ = nullptr;
    IntelTab *intel_ = nullptr;
    NvidiaTab *nvidia_ = nullptr;
    AmdGpuTab *amdgpu_ = nullptr;
    OptimizeTab *optimize_;
    FwattrTab *fwattr_ = nullptr;
    LightingTab *lighting_ = nullptr;
    SceneEngine *scenes_;
    GlobalShortcuts *shortcuts_;
    HealthTab *health_;
    LauncherWatch *launchers_ = nullptr;
    DriftWatch *drift_ = nullptr;
    SmartWatch *smart_ = nullptr;
    LidWatch *lid_ = nullptr;
    bool forceQuit_ = false, hideOnClose_ = false, wizardChecked_ = false;
};
