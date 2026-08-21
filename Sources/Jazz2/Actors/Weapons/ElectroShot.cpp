#include "ElectroShot.h"
#include "../../ILevelHandler.h"
#include "../../Events/EventMap.h"
#include "../Enemies/EnemyBase.h"
#include "../Explosion.h"
#include "../Player.h"

#include "../../../nCine/Base/Random.h"

using namespace Jazz2::Tiles;

namespace Jazz2::Actors::Weapons
{
	ElectroShot::ElectroShot()
		: _fired(0)
	{
	}

	Task<bool> ElectroShot::OnActivatedAsync(const ActorActivationDetails& details)
	{
		async_await ShotBase::OnActivatedAsync(details);

		_upgrades = details.Params[0];
		_strength = 4;
		_timeLeft = 55;

		SetState(ActorState::SkipPerPixelCollisions, true);
		SetState(ActorState::ApplyGravitation, false);

		async_await RequestMetadataAsync("Weapon/Electro"_s);

		AnimState state = AnimState::Idle;
		if ((_upgrades & 0x01) != 0) {
			state |= (AnimState)1;
		}
		SetAnimation(state);
		PlaySfx("Fire"_s);

		async_return true;
	}

	void ElectroShot::OnFire(const std::shared_ptr<ActorBase>& owner, Vector2f gunspotPos, Vector2f speed, float angle, bool isFacingLeft)
	{
		_owner = owner;
		SetFacingLeft(isFacingLeft);

		_gunspotPos = gunspotPos;

		float angleRel = angle * (isFacingLeft ? -1 : 1);

		float baseSpeed = ((_upgrades & 0x1) != 0 ? 5.0f : 4.0f);
		if (isFacingLeft) {
			_speed.X = std::min(0.0f, speed.X) - cosf(angleRel) * baseSpeed;
		} else {
			_speed.X = std::max(0.0f, speed.X) + cosf(angleRel) * baseSpeed;
		}
		_speed.Y = sinf(angleRel) * baseSpeed;

		_renderer.setRotation(angle);
		_renderer.setDrawEnabled(false);
	}

	void ElectroShot::OnUpdate(float timeMult)
	{
		std::int32_t n = (timeMult > 0.9f ? 2 : 1);
		TileCollisionParams params = { TileDestructType::Weapon | TileDestructType::IgnoreSolidTiles, false, WeaponType::Electro, _strength };
		for (std::int32_t i = 0; i < n && params.WeaponStrength > 0; i++) {
			TryMovement(timeMult / n, params);
		}
		if (params.TilesDestroyed > 0) {
			if (auto* player = runtime_cast<Player>(_owner.get())) {
				player->AddScore(params.TilesDestroyed * 50);
			}
		}
		if (params.WeaponStrength <= 0) {
			DecreaseHealth(INT32_MAX);
			return;
		}

		ShotBase::OnUpdate(timeMult);

		_fired++;
		if (_fired == 2) {
			MoveInstantly(_gunspotPos, MoveType::Absolute | MoveType::Force);
			_renderer.setDrawEnabled(true);
		}
	}

	void ElectroShot::OnUpdateHitbox()
	{
		UpdateHitbox(4, 4);
	}

	void ElectroShot::OnEmitLights(SmallVectorImpl<LightEmitter>& lights)
	{
		if (_fired >= 2) {
			auto& light = lights.emplace_back();
			light.Pos = _pos;
			light.Intensity = 0.8f;
			light.Brightness = 0.4f;
			light.RadiusNear = 0.0f;
			light.RadiusFar = 20.0f;
		}
	}

	bool ElectroShot::OnHandleCollision(ActorBase* other)
	{
		if (auto* enemyBase = runtime_cast<Enemies::EnemyBase>(other)) {
			if (enemyBase->IsInvulnerable() || !enemyBase->CanCollideWithShots) {
				return false;
			}
		}

		return ShotBase::OnHandleCollision(other);
	}

	bool ElectroShot::OnPerish(ActorBase* collider)
	{
		return ShotBase::OnPerish(collider);
	}

	void ElectroShot::OnHitWall(float timeMult)
	{
	}

	void ElectroShot::OnRicochet()
	{
	}
}
