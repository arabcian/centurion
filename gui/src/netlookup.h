#pragma once
// Health → Network → Lookup: on-demand questions about an address, name or port: whois (through the
// unprivileged netguard-helper), DNS records, a TCP port check, ping and traceroute. Nothing runs by itself.
#include <QJsonObject>
#include <QWidget>
#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

class LookupTab : public QWidget {
    Q_OBJECT
public:
    using HelperCall = std::function<void(const QJsonObject &request, std::function<void(const QJsonObject &)> done, int timeoutMs)>;
    explicit LookupTab(HelperCall helper, QWidget *parent = nullptr);
    /// Fills in `target` and runs the full whois lookup.
    void whois(const QString &target);

private:
    void toolChanged();
    void run();
    void stop();
    void finish(const QString &status);
    void runWhois(const QString &target);
    void runDns(const QString &target);
    void runPorts(const QString &target);
    void runCommand(const QString &program, const QStringList &args);

    HelperCall helper_;
    QComboBox *tool_;
    QLineEdit *target_, *extra_;
    QLabel *extraLabel_, *summary_, *state_;
    QCheckBox *routing_;
    QPushButton *run_, *stop_;
    QPlainTextEdit *output_;
    QObject *job_ = nullptr;  // owns everything the running lookup uses; deleting it cancels the lookup
    quint64 generation_ = 0;
};
