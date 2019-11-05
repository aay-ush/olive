#include "openglbackend.h"

#include <QEventLoop>
#include <QThread>

#include "functions.h"

OpenGLBackend::OpenGLBackend(QObject *parent) :
  VideoRenderBackend(parent),
  push_time_(-1),
  compiled_(false)
{
}

OpenGLBackend::~OpenGLBackend()
{
  CloseInternal();
}

bool OpenGLBackend::InitInternal()
{
  if (!VideoRenderBackend::InitInternal()) {
    return false;
  }

  QOpenGLContext* share_ctx = QOpenGLContext::currentContext();

  if (share_ctx == nullptr) {
    qCritical() << "No active OpenGL context to connect to";
    return false;
  }

  // Initiate one thread per CPU core
  for (int i=0;i<threads().size();i++) {
    QThread* thread = threads().at(i);

    // Create one processor object for each thread
    OpenGLWorker* processor = new OpenGLWorker(share_ctx, &shader_cache_, &decoder_cache_);
    processor->SetParameters(params());

    // Connect to it
    connect(processor, SIGNAL(RequestSibling(NodeDependency)), this, SLOT(ThreadRequestedSibling(NodeDependency)));
    connect(processor, SIGNAL(CompletedFrame(NodeDependency)), this, SLOT(ThreadCompletedFrame(NodeDependency)));

    // Finally, we can move it to its own thread
    processor->moveToThread(thread);

    // Add processor to list
    processors_.append(processor);

    // This function blocks the main thread intentionally. See the documentation for this function to see why.
    processor->Init();
  }

  // Create master texture (the one sent to the viewer)
  master_texture_ = std::make_shared<OpenGLTexture>();
  master_texture_->Create(share_ctx, params().effective_width(), params().effective_height(), params().format());

  // Create internal FBO for copying textures
  copy_buffer_.Create(share_ctx);
  copy_buffer_.Attach(master_texture_);
  copy_pipeline_ = OpenGLShader::CreateDefault();

  return true;
}

void OpenGLBackend::GenerateFrame(const rational &time)
{
  qDebug() << "Compiled state:" << compiled_;
  if (!compiled_) {
    Compile();
  }

  NodeDependency dep = NodeDependency(viewer_node()->texture_input()->get_connected_output(), time, time);

  foreach (OpenGLWorker* worker, processors_) {
    if (worker->IsAvailable() || worker == processors_.last()) {
      QMetaObject::invokeMethod(worker,
                                "Render",
                                Qt::QueuedConnection,
                                Q_ARG(NodeDependency, dep));
    }
  }
}

void OpenGLBackend::CloseInternal()
{
  Decompile();

  copy_buffer_.Destroy();
  master_texture_ = nullptr;
  copy_pipeline_ = nullptr;

  VideoRenderBackend::Close();
}

OpenGLTexturePtr OpenGLBackend::GetCachedFrameAsTexture(const rational &time)
{
  last_time_requested_ = time;

  if (push_time_ >= 0) {
    rational temp_push_time = push_time_;
    push_time_ = -1;

    if (time == temp_push_time) {
      return master_texture_;
    }
  }

  const char* cached_frame = GetCachedFrame(time);

  if (cached_frame != nullptr) {
    master_texture_->Upload(cached_frame);

    return master_texture_;
  }

  return nullptr;
}

bool OpenGLBackend::CompileInternal()
{
  if (viewer_node() == nullptr || !viewer_node()->texture_input()->IsConnected()) {
    // Nothing to be done, nothing to compile
    return true;
  }

  // Traverse node graph compiling where necessary
  bool ret = TraverseCompiling(viewer_node());

  if (ret) {
    qDebug() << "Compiled successfully!";
    compiled_ = true;
  } else {
    qDebug() << "Compile failed:" << GetError();
    Decompile();
  }

  return ret;
}

void OpenGLBackend::DecompileInternal()
{
  shader_cache_.Clear();
  compiled_ = false;
}

bool OpenGLBackend::TraverseCompiling(Node *n)
{
  foreach (NodeParam* param, n->parameters()) {
    if (param->type() == NodeParam::kInput && param->IsConnected()) {
      NodeOutput* connected_output = static_cast<NodeInput*>(param)->get_connected_output();

      // Check if we have a shader or not
      if (shader_cache_.GetShader(connected_output) == nullptr)  {
        // Since we don't have a shader, compile one now
        QString node_code = connected_output->parent()->Code(connected_output);

        // If the node has no code, it mustn't be GPU accelerated
        if (!node_code.isEmpty()) {
          // Since we have shader code, compile it now
          OpenGLShaderPtr program;

          if (!(program = std::make_shared<OpenGLShader>())) {
            SetError(QStringLiteral("Failed to create OpenGL shader object"));
            return false;
          }

          if (!program->create()) {
            SetError(QStringLiteral("Failed to create OpenGL shader on device"));
            return false;
          }

          if (!program->addShaderFromSourceCode(QOpenGLShader::Fragment, node_code)) {
            SetError(QStringLiteral("Failed to add OpenGL fragment shader code"));
            return false;
          }

          if (!program->addShaderFromSourceCode(QOpenGLShader::Vertex, OpenGLShader::CodeDefaultVertex())) {
            SetError(QStringLiteral("Failed to add OpenGL vertex shader code"));
            return false;
          }

          if (!program->link()) {
            SetError(QStringLiteral("Failed to compile OpenGL shader: %1").arg(program->log()));
            return false;
          }

          shader_cache_.AddShader(connected_output, program);

          qDebug() << "Compiled" <<  connected_output->parent()->id() << "->" << connected_output->id();
        }
      }

      if (!TraverseCompiling(connected_output->parent())) {
        return false;
      }
    }
  }

  return true;
}

void OpenGLBackend::ThreadCompletedFrame(NodeDependency path)
{
  caching_ = false;

  OpenGLTexturePtr texture = path.node()->get_cached_value(path.range()).value<OpenGLTexturePtr>();
  qDebug() << "Retrieved texture for time" << path.in();

  if (texture != nullptr) {
    QString cache_fn = CachePathName(QStringLiteral("%1-%2").arg(QString::number(path.in().numerator()), QString::number(path.in().denominator())).toLatin1());

    // Find an available worker to download this texture
    foreach (OpenGLWorker* worker, processors_) {
      // Check if one is available, but worst case if none of them are available, just queue it on the last worker since
      // it's the least likely to get work
      if (worker->IsAvailable() || worker == processors_.last()) {
        QMetaObject::invokeMethod(worker,
                                  "Download",
                                  Q_ARG(NodeDependency, path),
                                  Q_ARG(QVariant, QVariant::fromValue(texture)),
                                  Q_ARG(QString, cache_fn));
        break;
      }
    }
  }

  CacheNext();
}

void OpenGLBackend::ThreadCallback(OpenGLTexturePtr texture, const rational& time, const QByteArray& hash)
{
  // Threads are all done now, time to proceed
  caching_ = false;

  DeferMap(time, hash);

  if (texture != nullptr) {
    // We received a texture, time to start downloading it
    QString fn = CachePathName(hash);

    /*download_threads_[last_download_thread_%download_threads_.size()]->Queue(texture,
                                                                             fn,
                                                                             hash);*/
  } else {
    // There was no texture here, we must update the viewer
    DownloadThreadComplete(hash);
  }

  // If the connected output is using this time, signal it to update
  if (last_time_requested_ == time) {

    copy_buffer_.Bind();

    QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();

    if (texture == nullptr) {

      // No texture, clear the master and push it
      f->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
      f->glClear(GL_COLOR_BUFFER_BIT);

    } else {
      texture->Bind();

      f->glViewport(0, 0, master_texture_->width(), master_texture_->height());

      olive::gl::Blit(copy_pipeline_);

      texture->Release();
    }

    copy_buffer_.Release();

    push_time_ = time;

    emit CachedFrameReady(time);
  }

  CacheNext();
}

void OpenGLBackend::ThreadRequestedSibling(NodeDependency dep)
{
  // Try to queue another thread to run this dep in advance
  foreach (OpenGLWorker* worker, processors_) {
    if (worker->IsAvailable()) {
      QMetaObject::invokeMethod(worker,
                                "RenderAsSibling",
                                Qt::QueuedConnection,
                                Q_ARG(NodeDependency, dep));
      return;
    }
  }
}

void OpenGLBackend::ThreadSkippedFrame(const rational& time, const QByteArray& hash)
{
  caching_ = false;

  DeferMap(time, hash);

  if (!IsCaching(hash)) {
    DownloadThreadComplete(hash);

    // Signal output to update value
    emit CachedFrameReady(time);
  }

  CacheNext();
}

void OpenGLBackend::DownloadThreadComplete(const QByteArray &hash)
{
  cache_hash_list_mutex_.lock();
  cache_hash_list_.removeAll(hash);
  cache_hash_list_mutex_.unlock();

  for (int i=0;i<deferred_maps_.size();i++) {
    const HashTimeMapping& deferred = deferred_maps_.at(i);

    if (deferred_maps_.at(i).hash == hash) {
      // Insert into hash map
      time_hash_map_.insert(deferred.time, deferred.hash);

      deferred_maps_.removeAt(i);
      i--;
    }
  }
}