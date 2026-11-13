// M6 review L13: the recovery offer for an earlier run's recordings (pq/views/shell/recover_recordings.h).

#include "pq/views/shell/recover_recordings.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <memory>
#include <utility>

#include "pq/views/shell/shell.h"

namespace pq::views {

using namespace pq::ui;

RecoverRecordingsPopover::RecoverRecordingsPopover(ViewContext& ctx, std::vector<ViewContext::StaleRecordings> stale)
    : Popover("recover-recordings"), ctx_(ctx), stale_(std::move(stale)) {
  setSize(Length::fixed(kWidth), Length::fixed(kHeight));
  setPadding(Insets::all(10));
  title_ = &add<Label>("title", "", FontRoleId::BodyStrong, TextTone::Primary);
  title_->setHeight(Length::fixed(20));
  line1_ = &add<Label>("what", "", FontRoleId::Small, TextTone::Secondary);
  line1_->setHeight(Length::fixed(16));
  line2_ = &add<Label>("kept", "", FontRoleId::Small, TextTone::Secondary);
  line2_->setHeight(Length::fixed(16));
  add<Widget>("space").setHeight(Length::flex(1));
  auto& row = add<Widget>("buttons");
  row.setLayout(LayoutKind::Row);
  row.setGap(6);
  row.setHeight(Length::fixed(22));
  auto button = [&row](const char* name, const char* text, float w, const char* info, auto fn) {
    Button& b = row.add<Button>(name, text);
    b.setSize(Length::fixed(w), Length::fixed(22));
    b.setFocusable(false);
    b.setInfo(text, info);
    b.onClick = std::move(fn);
    return &b;
  };
  open_ = button("open", "Open as Project", 116.0f,
                 "A new project with these takes one after another on an audio track (asks to save the current one "
                 "first). Its first save moves them into the project folder.",
                 [this] { openAsProject(); });
  recover_ = button("recover", "Recover", 80.0f,
                    "Opens the autosaved document as it was, unsaved (asks to save the current one first). An untitled "
                    "one gets its recordings back.",
                    [this] { recover(); });
  button("reveal", "Show in Explorer", 116.0f, "Shows the folder with the takes. Nothing is changed.", [this] { reveal(); });
  row.add<Widget>("gap").setWidth(Length::flex(1));
  button("discard", "Discard", 70.0f, "Deletes the autosave and the takes of that session, and their folder.",
         [this] { discard(); });
  button("later", "Later", 60.0f, "Keeps them; they are offered again at the next start.", [this] { later(); });
  showCurrent();
}

void RecoverRecordingsPopover::showCurrent() {
  if (stale_.empty()) return;
  const ViewContext::StaleRecordings& s = stale_.front();
  const bool autosave = s.autosave.has_value();
  open_->setVisible(!autosave);
  recover_->setVisible(autosave);
  char buf[320];
  if (autosave) {  // AS.2
    title_->setText("pqdaw did not close properly");
    char when[64] = "at an unknown time";
    const std::time_t t = static_cast<std::time_t>(s.autosave->savedAt), now = std::time(nullptr);
    std::tm tm{}, today{};
    if (localtime_s(&tm, &t) == 0) {
      const bool sameDay = localtime_s(&today, &now) == 0 && today.tm_yday == tm.tm_yday && today.tm_year == tm.tm_year;
      std::strftime(when, sizeof when, sameDay ? "%H:%M" : "%d %b %Y, %H:%M", &tm);
    }
    std::string takes;
    if (!s.files.empty())
      takes = " (" + std::to_string(s.files.size()) + (s.files.size() == 1 ? " take" : " takes") + ")";
    std::snprintf(buf, sizeof buf, "%s, autosaved %s%s.", s.autosave->name.c_str(), when, takes.c_str());
    line1_->setText(buf);
  } else {
    title_->setText("Recordings from an earlier session");
    char when[64] = "an unknown time";
    const std::time_t t = static_cast<std::time_t>(s.started);
    std::tm tm{};
    if (localtime_s(&tm, &t) == 0) std::strftime(when, sizeof when, "%d %b %Y, %H:%M", &tm);
    std::snprintf(buf, sizeof buf, "pqdaw closed on %s without saving %zu take%s (%.1f MB).", when, s.files.size(),
                  s.files.size() == 1 ? "" : "s", static_cast<double>(s.bytes) / (1024.0 * 1024.0));
    line1_->setText(buf);
  }
  const std::string kept = autosave ? "It is kept until you choose" : "They are kept until you choose";
  line2_->setText(stale_.size() > 1 ? kept + " (" + std::to_string(stale_.size() - 1) + " more session" +
                                          (stale_.size() > 2 ? "s" : "") + " after this)."
                                    : kept + ".");
}

void RecoverRecordingsPopover::next() {
  if (!stale_.empty()) stale_.erase(stale_.begin());
  if (stale_.empty()) {
    if (OverlayLayer* layer = OverlayLayer::of(*this)) layer->close(*this);  // destroys this
    return;
  }
  showCurrent();
}

void RecoverRecordingsPopover::openAsProject() {
  if (stale_.empty()) return;
  // By value: after "Save" an untitled project's Save As dialog answers later (this popover may be gone by then).
  ViewContext* ctx = &ctx_;
  auto proceeded = std::make_shared<bool>(false);
  ctx_.guardUnsaved([ctx, s = stale_.front(), proceeded] {
    *proceeded = true;
    ctx->newProject();
    (void)ctx->recoverStaleRecordings(s);
  });
  if (*proceeded) next();  // cancelled at "save changes?" (or a Save As still open): still offered
}

void RecoverRecordingsPopover::recover() {
  if (stale_.empty() || !stale_.front().autosave) return;
  ViewContext* ctx = &ctx_;  // by value, as openAsProject
  auto proceeded = std::make_shared<bool>(false);
  ctx_.guardUnsaved([ctx, s = stale_.front(), proceeded] {
    *proceeded = true;
    (void)ctx->recoverAutosave(s);
  });
  if (*proceeded) next();
}

void RecoverRecordingsPopover::reveal() {
  if (stale_.empty()) return;
  if (FileDialogs* d = ctx_.fileDialogs()) d->showInExplorer(stale_.front().dir);
}

void RecoverRecordingsPopover::discard() {
  if (stale_.empty()) return;
  (void)ctx_.discardStaleRecordings(stale_.front());
  next();
}

void RecoverRecordingsPopover::later() {
  stale_.clear();
  next();
}

InfoText RecoverRecordingsPopover::info() const {
  if (!stale_.empty() && stale_.front().autosave)
    return {"Recover", "An earlier session ended without closing properly; its document was autosaved. Nothing is "
                       "deleted unless you choose Discard."};
  return {"Recover Recordings", "An earlier session ended without saving its untitled project; these takes were "
                                "recorded in it. Nothing is deleted unless you choose Discard."};
}

RecoverRecordingsPopover* offerRecordingRecovery(Shell& shell) {
  ViewContext& ctx = shell.context();
  std::vector<ViewContext::StaleRecordings> stale = ctx.staleRecordings();
  if (stale.empty()) return nullptr;
  auto pop = std::make_unique<RecoverRecordingsPopover>(ctx, std::move(stale));
  RecoverRecordingsPopover* raw = pop.get();
  OverlayOptions o;
  UiTree& tree = shell.tree();
  const UiScale s(tree.scale());
  const auto W = static_cast<int32_t>(tree.width()), H = static_cast<int32_t>(tree.height());
  o.anchor = {std::max(0, (W - s.px(RecoverRecordingsPopover::kWidth)) / 2), std::max(0, (H - s.px(RecoverRecordingsPopover::kHeight)) / 3), 0, 0};
  o.placement = Placement::AtPoint;
  o.within = tree.find("main/body");
  o.dismissOnOutsideClick = false;  // a choice, not a menu: a stray click does not dismiss it
  shell.overlay().open(std::move(pop), std::move(o));
  return raw;
}

}  // namespace pq::views
