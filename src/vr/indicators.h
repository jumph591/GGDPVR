#pragma once
#include <cstdint>
#include <string>

#include "../xr/xr_shared.h"
#include "vrmath.h"

// Off-screen indicators (turning=snap stages, Gal*Gun 2 style): a small chevron for each live girl outside the view,
// on a ring around the view's centre, pointing toward where she really is (up / down too); nothing while she is in
// view. Optionally a faint marker of another shape and tint for where the game camera looks, while that is out of
// view. The game thread gathers the girls and places the marks (head-locked, at the HUD's depth); the Composite pass
// projects them into each eye.
namespace ggdpvr::vr::ind {
// Game thread, once per frame before the eye draws: the girls for frame `frameId` (active: comfort turning is in
// charge of a stage's view). hs: that frame's head sample (head and eye poses, FOVs).
void Tick(uint64_t frameId, bool active, const xr::HeadSample& hs, bool headOk);
// PostGetPlayerViewPoint: this frame's mapping from the world to the recentred tracking space - the game camera's
// location (shake removed), the frame the head pose goes onto, and the game camera's own rotation. The marks of the
// frame are placed on its first call.
void SetFrame(uint64_t frameId, const V3& loc, const Rot& frame, const Rot& gameCam);

// Render thread: the marks of frame `frameId`, in the recentred tracking space (metres): each at `at`, pointing toward
// `tip`. marker: the game-look marker (only one, if any).
struct Mark {
    float at[3], tip[3];
};
constexpr int kMaxMarks = 16;
int MarksForFrame(uint64_t frameId, Mark* out, int max, bool& haveMarker, Mark& marker);
bool AnyRecent();  // marks were (or just were) up: the Composite pass must run
// Game thread: a point the player is asked to look at (world, uu): a chevron toward it while it is out of view (also
// with indicators=0); off = none. AttentionLooked: it was within 35 deg of the head's forward on the last frame placed.
void SetAttention(bool on, const V3& world);
bool AttentionLooked();

void DevDumpGirls();                          // dev: !girls - every zako girl's state, place, flags
void DevDefeatLive(const std::string& how);    // dev: !bombgirls [state] - every live girl: the Doki bomb (or that state)
}  // namespace ggdpvr::vr::ind
