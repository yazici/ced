#pragma once

#include <memory>
#include "buffer.h"
#include "diagnostic.h"

class LibClangCollaborator final : public SyncCollaborator {
 public:
  LibClangCollaborator(const Buffer* buffer);
  ~LibClangCollaborator();

  EditResponse Edit(const EditNotification& notification) override;

 private:
  const Buffer* const buffer_;
  UMapEditor<ID, Annotation<Token>> token_editor_;
  DiagnosticEditor diagnostic_editor_;
};
