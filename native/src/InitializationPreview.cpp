#include "InitializationPreview.h"
#include "AppLanguage.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace gsw {
namespace {
using Vec = std::array<double,3>;
bool vector(const QJsonValue &value, Vec *out) {
  const auto row = value.toArray(); if (row.size()!=3) return false;
  for(int i=0;i<3;++i) { if(!row[i].isDouble()||!std::isfinite(row[i].toDouble()))return false;(*out)[i]=row[i].toDouble(); }
  return true;
}
Vec subtract(Vec value,const Vec &origin) { for(int i=0;i<3;++i)value[i]-=origin[i]; return value; }
struct View { QString id,split; Vec center; std::array<Vec,3> rotation; std::array<double,4> k; int width=0,height=0; double skew01=0,skew10=0; };
QColor viewColor(const QString &split) { return split==QStringLiteral("train") ? QColor(46,116,207) : split==QStringLiteral("val") ? QColor(206,139,38) : QColor(143,93,190); }
}
class InitializationPreview::State {
public:
  QList<Vec> points,corners; QList<View> views; QString projection=QStringLiteral("isometric"), selected;
  bool valid=false; double yaw=-.6,pitch=.65,zoom=1.; QPointF last,pan;
};
InitializationPreview::InitializationPreview(QWidget *parent):QWidget(parent),m(std::make_unique<State>()) {
  setObjectName(QStringLiteral("initializationRelationshipPreview")); setMinimumSize(260,220);
  setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
  AppLanguage::bind(this,"toolTip",AppLanguage::source("初始化关系示意：拖动旋转，滚轮缩放。仅绘制实际点的显示子集；视锥绘制长度不是物体深度，可见性统计由后台完整点集计算。"));
  AppLanguage::onChanged(this,[this]{update();});
}
InitializationPreview::~InitializationPreview()=default;
bool InitializationPreview::setReport(const QJsonObject &report) {
  m->points.clear();m->corners.clear();m->views.clear();m->valid=false;m->pan={};m->zoom=1.;
  const auto previewData=report.value(QStringLiteral("preview")).toObject(); Vec origin{};
  if(previewData.value(QStringLiteral("frame"))!=QStringLiteral("canonical")||!vector(previewData.value(QStringLiteral("display_origin")),&origin)){update();return false;}
  const auto points=previewData.value(QStringLiteral("points")).toArray(), corners=previewData.value(QStringLiteral("bounds_corners")).toArray(), views=previewData.value(QStringLiteral("views")).toArray();
  if(points.size()>2048||(!corners.isEmpty()&&corners.size()!=8)||views.size()>4096){update();return false;}
  for(const auto &entry:points){Vec p{};if(!vector(entry,&p)){update();return false;}m->points.append(subtract(p,origin));}
  for(const auto &entry:corners){Vec p{};if(!vector(entry,&p)){update();return false;}m->corners.append(subtract(p,origin));}
  for(const auto &entry:views) {
    const auto item=entry.toObject();const auto pose=item.value(QStringLiteral("c2w_opencv")).toArray(), k=item.value(QStringLiteral("K")).toArray();
    if(pose.size()!=4||k.size()!=3){update();return false;}
    View view;view.id=item.value(QStringLiteral("view_id")).toString();view.split=item.value(QStringLiteral("split")).toString();
    view.width=item.value(QStringLiteral("width")).toInt();view.height=item.value(QStringLiteral("height")).toInt();
    if(view.id.isEmpty()||view.width<=0||view.height<=0){update();return false;}
    for(int r=0;r<4;++r) {
      const auto row=pose[r].toArray();if(row.size()!=4){update();return false;}
      for(int c=0;c<4;++c)if(!row[c].isDouble()||!std::isfinite(row[c].toDouble())){update();return false;}
      if(r<3){for(int c=0;c<3;++c)view.rotation[r][c]=row[c].toDouble();view.center[r]=row[3].toDouble()-origin[r];}
    }
    for(const auto &rowValue:k){Vec row{};if(!vector(rowValue,&row)){update();return false;}}
    view.k={k[0].toArray()[0].toDouble(),k[1].toArray()[1].toDouble(),k[0].toArray()[2].toDouble(),k[1].toArray()[2].toDouble()};
    view.skew01=k[0].toArray()[1].toDouble();view.skew10=k[1].toArray()[0].toDouble();
    if(view.k[0]<=0||view.k[1]<=0||std::abs(view.k[0]*view.k[1]-view.skew01*view.skew10)<1e-15){update();return false;}
    m->views.append(view);
  }
  m->valid=true;update();return true;
}
void InitializationPreview::setProjection(const QString &projection) {
  if(projection!=QStringLiteral("isometric")&&projection!=QStringLiteral("xy")&&projection!=QStringLiteral("xz")&&projection!=QStringLiteral("yz"))return;
  m->projection=projection;update();
}
void InitializationPreview::setSelectedView(const QString &viewId){m->selected=viewId;update();}
void InitializationPreview::paintEvent(QPaintEvent *) {
  QPainter painter(this);painter.fillRect(rect(),palette().brush(QPalette::Base));painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(palette().color(QPalette::Text));
  if(!m->valid){painter.drawText(rect().adjusted(12,12,-12,-12),Qt::AlignCenter|Qt::TextWordWrap,QCoreApplication::translate("Workbench","完成初始化检查后显示相机、粗范围和实际初始化点的关系；这不是模型表面预览。"));return;}
  const auto project=[this](const Vec &p){
    if(m->projection==QStringLiteral("xy"))return QPointF(p[0],-p[1]);
    if(m->projection==QStringLiteral("xz"))return QPointF(p[0],-p[2]);
    if(m->projection==QStringLiteral("yz"))return QPointF(p[1],-p[2]);
    const double x=std::cos(m->yaw)*p[0]-std::sin(m->yaw)*p[1];
    const double y=std::sin(m->yaw)*p[0]+std::cos(m->yaw)*p[1];
    return QPointF(x,std::sin(m->pitch)*y-std::cos(m->pitch)*p[2]);
  };
  QList<Vec> all=m->points;all.append(m->corners);for(const auto &view:m->views)all.append(view.center);
  if(all.isEmpty())return;
  Vec minimum=all.first(),maximum=all.first();for(const auto &p:all)for(int i=0;i<3;++i){minimum[i]=std::min(minimum[i],p[i]);maximum[i]=std::max(maximum[i],p[i]);}
  double extent=0;for(int i=0;i<3;++i)extent+=std::pow(maximum[i]-minimum[i],2.);extent=std::sqrt(extent);
  // The small displayed frustum is directional notation only. It is not a
  // guessed scene-depth bound or a substitute for the helper's coverage gate.
  const double frustumDepth=extent>0?extent*.09:1.;
  QList<std::array<Vec,4>> frustums;
  for(const auto &view:m->views) {
    std::array<Vec,4> corners{};const std::array<QPointF,4> pixels{QPointF(0,0),QPointF(view.width,0),QPointF(view.width,view.height),QPointF(0,view.height)};
    const double det=view.k[0]*view.k[1]-view.skew01*view.skew10;
    for(int i=0;i<4;++i){const double u=pixels[i].x()-view.k[2],v=pixels[i].y()-view.k[3];
      const Vec local{frustumDepth*(view.k[1]*u-view.skew01*v)/det,frustumDepth*(-view.skew10*u+view.k[0]*v)/det,frustumDepth};
      corners[i]=view.center;for(int r=0;r<3;++r)for(int c=0;c<3;++c)corners[i][r]+=view.rotation[r][c]*local[c];all.append(corners[i]);}
    frustums.append(corners);
  }
  QRectF bounds;bool first=true;for(const auto &p:all){const auto point=project(p);if(first){bounds=QRectF(point,QSizeF(0,0));first=false;}else bounds=bounds.united(QRectF(point,QSizeF(0.001,0.001)));}
  const double scale=std::min((width()-44.)/std::max(bounds.width(),1e-12),(height()-64.)/std::max(bounds.height(),1e-12))*m->zoom;
  const QPointF screenCenter=rect().center()+m->pan+QPointF(0,8);
  const auto screen=[&](const Vec &p){return screenCenter+(project(p)-bounds.center())*scale;};
  painter.setPen(QPen(palette().color(QPalette::Mid),1.2));
  if(m->corners.size()==8)for(int i=0;i<8;++i)for(int bit:{1,2,4})if(i<(i^bit))painter.drawLine(screen(m->corners[i]),screen(m->corners[i^bit]));
  painter.setPen(Qt::NoPen);painter.setBrush(QColor(44,139,124));for(const auto &p:m->points)painter.drawEllipse(screen(p),1.7,1.7);
  for(qsizetype i=0;i<m->views.size();++i){const auto &view=m->views[i];const auto &corners=frustums[i];const bool selected=view.id==m->selected;
    const QColor color=selected?palette().color(QPalette::Highlight):viewColor(view.split);painter.setPen(QPen(color,selected?2.5:1.2));
    for(int c=0;c<4;++c){painter.drawLine(screen(view.center),screen(corners[c]));painter.drawLine(screen(corners[c]),screen(corners[(c+1)%4]));}
    Vec forward=view.center;for(int r=0;r<3;++r)forward[r]+=view.rotation[r][2]*frustumDepth*1.4;
    const QPointF a=screen(view.center),b=screen(forward);painter.drawLine(a,b);const QLineF shaft(a,b);
    if(shaft.length()>3){const auto angle=std::atan2(b.y()-a.y(),b.x()-a.x());painter.setBrush(color);painter.drawPolygon(QPolygonF{b,b-QPointF(std::cos(angle-.5),std::sin(angle-.5))*7.,b-QPointF(std::cos(angle+.5),std::sin(angle+.5))*7.});}
    painter.setBrush(color);painter.drawEllipse(a,selected?4.:3.,selected?4.:3.);
  }
  painter.setPen(palette().color(QPalette::Text));painter.drawText(rect().adjusted(10,5,-10,-5),Qt::AlignTop|Qt::TextWordWrap,
      QCoreApplication::translate("Workbench","规范坐标关系示意 · 显示 %1 个实际点 · %2 个相机").arg(m->points.size()).arg(m->views.size()));
}
void InitializationPreview::mousePressEvent(QMouseEvent *event){m->last=event->position();event->accept();}
void InitializationPreview::mouseMoveEvent(QMouseEvent *event){const auto delta=event->position()-m->last;m->last=event->position();
  if(event->buttons().testFlag(Qt::LeftButton)&&m->projection==QStringLiteral("isometric")){m->yaw+=delta.x()*.01;m->pitch=std::clamp(m->pitch+delta.y()*.01,-1.5,1.5);update();}
  else if(event->buttons().testFlag(Qt::RightButton)){m->pan+=delta;update();}event->accept();}
void InitializationPreview::wheelEvent(QWheelEvent *event){m->zoom=std::clamp(m->zoom*std::exp(event->angleDelta().y()*.001),.1,20.);update();event->accept();}
}
