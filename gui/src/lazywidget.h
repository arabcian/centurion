#pragma once
// Placeholder page that builds its real widget on first show. For tabs that
// are expensive to construct and rarely opened: nothing of theirs runs at
// login while the app sits in the tray.
#include <QShowEvent>
#include <QVBoxLayout>
#include <QWidget>
#include <functional>

class LazyWidget : public QWidget {
public:
    explicit LazyWidget(std::function<QWidget *()> make, QWidget *parent = nullptr)
        : QWidget(parent), make_(std::move(make)) {
        auto *l = new QVBoxLayout(this);
        l->setContentsMargins(0, 0, 0, 0);
    }

    /// The real widget, built now if it was never shown (for a caller that must talk to it first).
    QWidget *content() {
        build();
        return content_;
    }

protected:
    void showEvent(QShowEvent *e) override {
        build();
        QWidget::showEvent(e);
    }

private:
    void build() {
        if (!make_) return;
        content_ = make_();
        make_ = nullptr;
        layout()->addWidget(content_);
    }
    std::function<QWidget *()> make_;
    QWidget *content_ = nullptr;
};
