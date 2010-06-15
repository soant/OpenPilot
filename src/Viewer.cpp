#include "qdisplay/Viewer.hpp"

#include <QGraphicsScene>
#include <QKeyEvent>
#include <QSplitter>
#include <QWheelEvent>
#include <QSvgGenerator>
#include <QMenu>

#include <QPrinter>
#include <QPainter>
#include <QFileDialog>
#include <QMessageBox>


// #include <QtOpenGL/QGLWidget>

#include <kernel/jafarMacro.hpp>

#include "qdisplay/ImageView.hpp"
#include "qdisplay/Line.hpp"
#include "qdisplay/PolyLine.hpp"
#include "qdisplay/Shape.hpp"
#include "qdisplay/ViewerManager.hpp"

#include <QSplitter>

using namespace jafar::qdisplay;
  
Viewer::Viewer(int mosaicWidth, int mosaicHeight, QGraphicsScene* scene ) : m_scene(scene), m_mosaicWidth(mosaicWidth), m_mosaicHeight(mosaicHeight), m_currentZ(0.)
{
	m_windowWidth = -1;	// norman
	m_windowHeight = -1;	// norman
	
  if(not m_scene) {
    m_scene = new QGraphicsScene();
  }
  m_scene->setBackgroundBrush( Qt::white );
	setDragMode(QGraphicsView::ScrollHandDrag);
	setTransformationAnchor(QGraphicsView::AnchorUnderMouse); 
	// added by norman
	if ((m_windowHeight > 0) and (m_windowWidth > 0))
	{
		setGeometry(0,0,m_windowWidth,m_windowHeight);
	}
  show();
  setScene(m_scene);
  ViewerManager::registerViewer( this );
  m_exportView = new QAction("Export the view", (QObject*)this);
  connect(m_exportView, SIGNAL(triggered()), this, SLOT(exportView()));
}

Viewer::~Viewer()
{
  ViewerManager::unregisterViewer( this );
}

// added by norman
void Viewer::setWindowSize( int width, int height )
{
	m_windowWidth = width;
	m_windowHeight = height;
}

void Viewer::addShape(qdisplay::Shape* si) {
  scene()->addItem(si);
  si->setZValue(m_currentZ++);
}

void Viewer::addLine(qdisplay::Line* li) {
  scene()->addItem(li);
  li->setZValue(m_currentZ++);
}

void Viewer::addPolyLine(qdisplay::PolyLine* pl)
{
  scene()->addItem(pl);
  pl->setZValue(m_currentZ++);
}

void Viewer::setImageView(ImageView* ii, int row, int col)
{
	if (ii == NULL)
	{
		if (m_imageMosaic[row][col])
		{
			scene()->removeItem(m_imageMosaic[row][col]);
			delete m_imageMosaic[row][col];
			m_imageMosaic[row][col] = NULL;
		}
		return;
	}
	
  if(scene()->items().contains(ii)) return;
  scene()->addItem(ii);
  if(m_imageMosaic[row][col])
  {
    scene()->removeItem(m_imageMosaic[row][col]);
    delete m_imageMosaic[row][col];
  }
  m_imageMosaic[row][col] = ii;
  if(m_mosaicWidth == 0 && m_mosaicHeight == 0)
  {
    QRect imageArea = ii->boundingRect().toRect();
    m_mosaicWidth = imageArea.width() + 5;
    m_mosaicHeight = imageArea.height() + 5;
    resize( m_mosaicWidth, m_mosaicHeight );
  }
  ii->setPos( row * m_mosaicWidth, col*m_mosaicHeight);
	
	// added by norman
	if ((m_windowHeight > 0) and (m_windowWidth > 0))
	{
		setGeometry(0,0,m_windowWidth,m_windowHeight);
	}
}

int Viewer::rows()
{
  if( m_imageMosaic.size() == 0)
    return 0;
  int maxrows = 0;
  for(QMap< int, QMap< int, ImageView* > >::iterator it = m_imageMosaic.begin(); it != m_imageMosaic.end(); it++)
  {
    int rows = (--it.value().end()).key();
    if( rows > maxrows) maxrows = rows;
  }
  return maxrows + 1;
}

int Viewer::cols()
{
  if( m_imageMosaic.size() == 0)
    return 0;
  return (--m_imageMosaic.end()).key() + 1;
}

void Viewer::keyPressEvent ( QKeyEvent * event )
{
  switch (event->key()) {
    case Qt::Key_Plus:
      scaleView(1.2);
      break;
    case Qt::Key_Minus:
      scaleView(1 / 1.2);
      break;
  }
}

void Viewer::wheelEvent(QWheelEvent *event)
{
  scaleView(pow((double)2, event->delta() / 240.0));
}

void Viewer::scaleView(qreal scaleFactor)
{
  qreal factor = matrix().scale(scaleFactor, scaleFactor).mapRect(QRectF(0, 0, 1, 1)).width();
  if (factor < 0.07 || factor > 100)
    return;

  scale(scaleFactor, scaleFactor);
}

void Viewer::close()
{
  setVisible(false);
}

void Viewer::splitVertical()
{
  QSplitter* parentSplitter = dynamic_cast<QSplitter*>(parentWidget());
  QSplitter* s = new QSplitter(Qt::Vertical, parentWidget());
  if(parentSplitter)
  {
    parentSplitter->insertWidget(parentSplitter->indexOf(this), s);
  }
  Viewer* clone = new Viewer(0,0,scene());
  s->addWidget(this);
  s->addWidget(clone);
  s->setVisible(true);
}

void Viewer::splitHorizontal()
{
  QSplitter* parentSplitter = dynamic_cast<QSplitter*>(parentWidget());
  QSplitter* s = new QSplitter(Qt::Horizontal, parentWidget());
  if(parentSplitter)
  {
    parentSplitter->insertWidget(parentSplitter->indexOf(this), s);
  }
  Viewer* clone = new Viewer(0,0,scene());
  JFR_DEBUG(scene());
  JFR_DEBUG(scene()->views().size());
  s->addWidget(this);
  s->addWidget(clone);
  s->setVisible(true);
}

void Viewer::contextMenuEvent( QContextMenuEvent * event )
{
  if(itemAt(event->pos()) )
  {
    QGraphicsView::contextMenuEvent(event);
  } else {
    QMenu menu;
    menu.addAction(m_exportView);
    menu.exec(event->globalPos());
  }
}

// WORKAROUND Qt4.4 regression where mouseReleaseEvent are not forwarded to QGraphicsItem
// is this problem still there with Qt4.6 ? Disabling it because it is messing with
// DragMode ScrollHandDrag (not releasing the drag)
#include <QGraphicsSceneMouseEvent>
#include <QApplication>
#if 0
void Viewer::mouseReleaseEvent( QMouseEvent* event )
{
  
  if( QGraphicsItem* qgi = itemAt(event->pos() ) )
  {
    ImageView* iv = dynamic_cast<ImageView*>( qgi->group() );
    if( iv )
    {
      QGraphicsSceneMouseEvent mouseEvent(QEvent::GraphicsSceneMouseRelease);
      mouseEvent.setWidget(viewport());
      QPointF mousePressScenePoint = mapToScene(event->pos());
      mouseEvent.setButtonDownScenePos(event->button(), mousePressScenePoint);
      mouseEvent.setButtonDownScreenPos(event->button(), event->globalPos());
      mouseEvent.setScenePos(mapToScene(event->pos()));
      mouseEvent.setScreenPos(event->globalPos());
      mouseEvent.setButtons(event->buttons());
      mouseEvent.setButton(event->button());
      mouseEvent.setModifiers(event->modifiers());
      mouseEvent.setAccepted(false);
      mouseEvent.setPos( iv->mapFromScene( mouseEvent.scenePos() ) );
      iv->mouseReleaseEvent( &mouseEvent );
      return;
    }
  }
  QGraphicsView::mouseReleaseEvent( event );
}
#endif

void Viewer::exportView()
{
  QString fileName = QFileDialog::getSaveFileName ( 0, "Export viewer content", "", "PDF Document (*.pdf);;Postscript (*.ps);;PNG Image (*.png);;Tiff Image (*.tiff);;Scalable Vector Graphics (*.svg)" );
  if(fileName == "") return;
  exportView( fileName.toAscii().data() );
}
  
void Viewer::exportView( const std::string& _fileName )
{
  QString fileName = _fileName.c_str();
  QString extension = fileName.split(".").last().toLower();
  if(extension == "pdf" or extension == "ps")
  {
    QPrinter printer;
    printer.setOutputFileName(fileName);
    QSizeF sF = scene()->sceneRect().size().toSize();
    printer.setPageSize(QPrinter::Custom);
    printer.setPaperSize(QSizeF(sF.width(), sF.height() ), QPrinter::DevicePixel);
    printer.setPageMargins(0,0,0,0, QPrinter::DevicePixel);
    if(extension == "pdf") printer.setOutputFormat(QPrinter::PdfFormat);
    else printer.setOutputFormat(QPrinter::PostScriptFormat);
    QPainter painter(&printer);
    this->scene()->render(&painter);
  } else if ( extension == "png" or extension == "tiff" )
  {
    QImage img( scene()->sceneRect().size().toSize() , QImage::Format_RGB32);
    QPainter painter(&img);
    this->scene()->render(&painter);
    if( extension == "png")
    {
        img.save(fileName, "PNG", 100);
    }
    else {
        img.save(fileName, "TIFF", 100);
    }
  } else if ( extension == "svg" )
  {
    QSvgGenerator generator;
    generator.setFileName(fileName);
    generator.setSize(scene()->sceneRect().size().toSize());
    QPainter painter(&generator);
    this->scene()->render(&painter);
    painter.end();
  } else {
    QMessageBox::critical(0, "Unsupported format", "This format " + extension + " is unsupported by the viewer export");
  }
}

void Viewer::setTitle(const std::string& _title )
{
  setWindowTitle(_title.c_str());
}

#include "Viewer.moc"
