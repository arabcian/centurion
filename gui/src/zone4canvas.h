#pragma once
// Draft-only canvas; no helper, settings, or device APIs.
#include "lightingpreview.h"
#include <QWidget>
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QCoreApplication>
#include <array>
#include <functional>
class Zone4Canvas final : public QWidget {
public:
    explicit Zone4Canvas(QWidget *parent=nullptr):QWidget(parent) {
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(QCoreApplication::translate("Zone4Tab","Four-zone keyboard preview"));
        setMinimumHeight(fontMetrics().height()*5);
        setToolTip(QCoreApplication::translate("Zone4Tab","Click or use Left/Right to select a zone. Enter edits its color. Ctrl+wheel zooms; middle drag pans; Home fits."));
    }
    void setColors(const std::array<QColor,4>& colors){colors_=colors;update();}
    void selectZone(int i){selected_=std::clamp(i,0,3);update();}
    int selectedZone() const{return selected_;}
    void fit(){view_.reset();update();}
    void zoomBy(double factor){view_.zoomAt(factor,{.5,.5});update();}
    int hitZone(QPointF pos) const {
        auto r=board();if(r.width()<=0||r.height()<=0)return -1;
        return lightingpreview::zoneAt(view_,{(pos.x()-r.x())/r.width(),(pos.y()-r.y())/r.height()}).value_or(-1);
    }
    std::function<void(int)> editZone;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);
        const auto r=board();if(r.width()<=0||r.height()<=0)return;
        p.setClipRect(r);
        const auto point=[&](double x,double y){auto v=view_.map({x,y});return QPointF(r.x()+r.width()*v.x,r.y()+r.height()*v.y);};
        for(int i=0;i<4;++i){
            QRectF box(point(i/4.,0),point((i+1)/4.,1));
            p.setPen(QPen(i==selected_?palette().highlight().color():palette().mid().color(),i==selected_?3:1));
            p.setBrush(colors_[i]);p.drawRoundedRect(box.adjusted(2,2,-2,-2),5,5);
            const auto c=colors_[i];p.setPen(c.lightnessF()>.5?Qt::black:Qt::white);
            p.drawText(box.adjusted(4,4,-4,-4),Qt::AlignCenter,QString::number(i+1));
        }
    }
    void mousePressEvent(QMouseEvent*e) override {
        if(e->button()==Qt::MiddleButton){drag_=true;last_=e->position();e->accept();return;}
        if(e->button()==Qt::LeftButton){int i=hitZone(e->position());if(i>=0){selectZone(i);if(editZone)editZone(i);e->accept();return;}}
        QWidget::mousePressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent*e) override {
        const auto r=board();if(drag_&&r.width()>0&&r.height()>0){auto d=e->position()-last_;last_=e->position();view_.pan({d.x()/r.width(),d.y()/r.height()});update();e->accept();return;} QWidget::mouseMoveEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent*e) override {if(e->button()==Qt::MiddleButton)drag_=false;QWidget::mouseReleaseEvent(e);}
    void wheelEvent(QWheelEvent*e) override {
        if(!(e->modifiers()&Qt::ControlModifier)){e->ignore();return;}
        auto r=board();if(r.width()>0&&r.height()>0){auto pos=e->position();view_.zoomAt(std::pow(1.2,e->angleDelta().y()/120.),{(pos.x()-r.x())/r.width(),(pos.y()-r.y())/r.height()});update();}e->accept();
    }
    void keyPressEvent(QKeyEvent*e) override {
        switch(e->key()){
        case Qt::Key_Left: selectZone(selected_-1);break;
        case Qt::Key_Right:selectZone(selected_+1);break;
        case Qt::Key_Home:fit();break;
        case Qt::Key_Plus:case Qt::Key_Equal:zoomBy(1.2);break;
        case Qt::Key_Minus:zoomBy(1/1.2);break;
        case Qt::Key_Return:case Qt::Key_Enter:case Qt::Key_Space:if(editZone)editZone(selected_);break;
        default:QWidget::keyPressEvent(e);return;
        }e->accept();
    }
private:
    QRectF board() const {QRectF r=QRectF(rect()).adjusted(4,4,-4,-4);const double h=std::min(r.height(),r.width()/4.);return {r.x(),r.center().y()-h/2,r.width(),h};}
    canvasviewport::View view_;
    std::array<QColor,4> colors_{QColor(Qt::white),QColor(Qt::white),QColor(Qt::white),QColor(Qt::white)};
    int selected_=0;bool drag_=false;QPointF last_;
};
