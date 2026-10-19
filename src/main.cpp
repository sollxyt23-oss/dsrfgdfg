// Input Buffer: widens the classic mode input buffer.
//
// How it works:
//  - When a jump press comes in, it goes through the game normally. If the mod
//    is enabled, we also remember it as "pending" for (window - 1) more steps.
//  - Each physics step (processCommands), if the press hasn't been "consumed",
//    we re-fire it so it can land on an orb/pad/ground the moment it is valid.
//  - A press counts as consumed if the player's gravity flipped or got an
//    upward impulse (jump, orb, pad). Then the buffer clears so nothing
//    double-fires.
//  - If you release while a press is still buffered, the release is held back
//    until the buffer resolves, so a quick tap still has its full window.
//
// Classic mode only. Ship, wave, robot, spider and swing are skipped.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

using namespace geode::prelude;

static bool s_enabled = true;
static int  s_steps   = 4;

// Upward velocity change (gravity-relative) that we treat as "a jump/orb/pad
// just fired". Tune this if buffered presses double-fire or clear too late.
constexpr double kImpulseThreshold = 3.0;

$execute {
	s_enabled = Mod::get()->getSettingValue<bool>("enable-buffer");
	s_steps   = static_cast<int>(Mod::get()->getSettingValue<int64_t>("buffer-steps"));

	listenForSettingChanges<bool>("enable-buffer", [](bool value) {
		s_enabled = value;
	});
	listenForSettingChanges<int64_t>("buffer-steps", [](int64_t value) {
		s_steps = static_cast<int>(value);
	});
}

static bool isBufferableMode(PlayerObject* p) {
	if (!p) return false;
	return !(p->m_isShip || p->m_isDart || p->m_isRobot || p->m_isSpider || p->m_isSwing);
}

class $modify(BufferLayer, GJBaseGameLayer) {
	struct Fields {
		int    pending[2]          = {0, 0};      // extra steps left to re-fire (0 = P1, 1 = P2)
		bool   deferredRelease[2]  = {false, false};
		bool   releaseDue[2]       = {false, false};
		bool   replaying           = false;       // true while we inject input ourselves
		double lastVy[2]           = {0.0, 0.0};
		bool   lastUpsideDown[2]   = {false, false};
	};

	PlayerObject* playerFor(int idx) {
		return idx == 0 ? m_player1 : m_player2;
	}

	void snapshot(int idx) {
		auto f = m_fields.self();
		if (auto p = playerFor(idx)) {
			f->lastVy[idx] = p->m_yVelocity;
			f->lastUpsideDown[idx] = p->m_isUpsideDown;
		}
	}

	// Ends buffering for this player and sends any release we held back.
	void flushRelease(int idx) {
		auto f = m_fields.self();
		f->pending[idx] = 0;
		f->releaseDue[idx] = false;
		if (f->deferredRelease[idx]) {
			f->deferredRelease[idx] = false;
			f->replaying = true;
			this->handleButton(false, 1, idx == 0);
			f->replaying = false;
		}
	}

	void handleButton(bool down, int button, bool isPlayer1) {
		auto f = m_fields.self();
		int idx = isPlayer1 ? 0 : 1;

		// Our own injected input, or not the jump button: pass straight through.
		if (f->replaying || button != 1) {
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
			return;
		}

		// Release while a press is buffered: hold it back until the buffer resolves.
		if (!down && f->pending[idx] > 0) {
			f->deferredRelease[idx] = true;
			return;
		}

		// A fresh press: close out any previous buffered press first.
		if (down && (f->pending[idx] > 0 || f->releaseDue[idx])) {
			flushRelease(idx);
		}

		GJBaseGameLayer::handleButton(down, button, isPlayer1);

		if (down && s_enabled && s_steps > 1 && !m_levelSettings->m_platformerMode) {
			if (isBufferableMode(playerFor(idx))) {
				f->pending[idx] = s_steps - 1;
				f->deferredRelease[idx] = false;
				snapshot(idx);
			}
		}
	}

	void processCommands(float dt) {
		auto f = m_fields.self();

		for (int idx = 0; idx < 2; ++idx) {
			if (f->releaseDue[idx]) {
				flushRelease(idx);
			}
			if (f->pending[idx] <= 0) continue;

			auto p = playerFor(idx);
			if (!s_enabled || !isBufferableMode(p)) {
				flushRelease(idx);
				continue;
			}

			// Was the buffered press consumed since the last step?
			double lastDir = f->lastUpsideDown[idx] ? -1.0 : 1.0;
			double dir     = p->m_isUpsideDown ? -1.0 : 1.0;
			double prevVy  = f->lastVy[idx] * lastDir;
			double vy      = p->m_yVelocity * dir;

			bool flipped = p->m_isUpsideDown != f->lastUpsideDown[idx];
			bool impulse = vy > kImpulseThreshold && (vy - prevVy) > kImpulseThreshold;

			if (flipped || impulse) {
				flushRelease(idx);
				continue;
			}

			// Not consumed yet: re-fire the press for this step.
			snapshot(idx);
			f->replaying = true;
			this->handleButton(true, 1, idx == 0);
			f->replaying = false;

			if (--f->pending[idx] <= 0) {
				// Window used up. Let this last re-fire play out for a step,
				// then send the held-back release at the start of the next one.
				f->pending[idx] = 0;
				f->releaseDue[idx] = true;
			}
		}

		GJBaseGameLayer::processCommands(dt);
	}
};
