#include "Canvas.h"
#include "../ContentResolver.h"

#include "../../nCine/Graphics/RenderQueue.h"
#include "../../nCine/Base/Random.h"

namespace Jazz2::UI
{
	Canvas::Canvas()
		: AnimTime(0.0f), _renderCommandsCount(0), _currentRenderQueue(nullptr)
	{
		setVisitOrderState(SceneNode::VisitOrderState::Disabled);
	}

	void Canvas::OnUpdate(float timeMult)
	{
		SceneNode::OnUpdate(timeMult);

		AnimTime += timeMult * AnimTimeMultiplier;
	}

	bool Canvas::OnDraw(RenderQueue& renderQueue)
	{
		SceneNode::OnDraw(renderQueue);

		_renderCommandsCount = 0;
		_currentRenderQueue = &renderQueue;

		return false;
	}

	void Canvas::DrawRenderCommand(RenderCommand* command)
	{
		_currentRenderQueue->AddCommand(command);
	}

	void Canvas::DrawTexture(const Texture& texture, Vector2f pos, std::uint16_t z, Vector2f size, const Vector4f& texCoords, const Colorf& color, bool additiveBlending, float angle, std::int32_t paletteOffset)
	{
		// Apply the optional per-layer draw transform (menu section transitions; identity by default)
		pos = pos * LayerScale + LayerOffset;
		size = size * LayerScale;
		const Colorf finalColor = color * LayerColor;

		// An indexed texture (palette index in the red channel) is recolored at draw time through the shared palette
		// texture; paletteOffset selects the palette region (0 = default sprite palette). -1 = a plain RGBA texture.
		bool indexed = (paletteOffset >= 0);
		auto command = RentRenderCommand();
#if defined(DEATH_TARGET_PSP)
		{
		if (additiveBlending) command->GetMaterial().SetBlendingFactors(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
		else command->GetMaterial().SetBlendingFactors(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

		Matrix4x4f worldMatrix = Matrix4x4f::Translation(pos.X, pos.Y, 0.0f);
		if (std::abs(angle) > 0.01f) {
			worldMatrix.Translate(size.X * 0.5f, size.Y * 0.5f, 0.0f);
			worldMatrix.RotateZ(angle);
			worldMatrix.Translate(size.X * -0.5f, size.Y * -0.5f, 0.0f);
		}
		command->SetTransformation(worldMatrix);
		command->SetLayer((std::uint16_t)(z + RenderCommand::PspUiLayerBase));
		command->GetMaterial().SetTexture(0, texture);
		command->SetPspSpriteData(finalColor.Data(), texCoords.X, texCoords.Y, texCoords.Z, texCoords.W,
			size.X, size.Y, (indexed ? (float)paletteOffset : 0.0f));
		command->pspUiSpace_ = true;
		command->GetGeometry().SetDrawParameters(GL_TRIANGLE_STRIP, 0, 4);
		_currentRenderQueue->AddCommand(command);
		}
		return;
#endif
		bool shaderChanged = (indexed
			? command->GetMaterial().SetShader(ContentResolver::Get().GetShader(PrecompiledShader::PaletteRemap))
			: command->GetMaterial().SetShaderProgramType(Material::ShaderProgramType::Sprite));
		if (shaderChanged) {
			command->GetMaterial().ReserveUniformsDataMemory();
			command->GetGeometry().SetDrawParameters(GL_TRIANGLE_STRIP, 0, 4);

			auto* textureUniform = command->GetMaterial().Uniform(Material::TextureUniformName);
			if (textureUniform && textureUniform->GetIntValue(0) != 0) {
				textureUniform->SetIntValue(0); // GL_TEXTURE0
			}
			auto* paletteUniform = command->GetMaterial().Uniform("uTexturePalette");
			if (paletteUniform != nullptr) {
				paletteUniform->SetIntValue(1); // GL_TEXTURE1
			}
		}

		// Use a separate alpha blend so the alpha channel of an RGBA render target accumulates correct "over" coverage,
		// otherwise drawing semi-transparent content over an opaque background would erode its alpha and let the
		// scene behind show through. Harmless for opaque/RGB targets (alpha ignored).
		if (additiveBlending) {
			command->GetMaterial().SetBlendingFactors(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
		} else {
			command->GetMaterial().SetBlendingFactors(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		}

		auto instanceBlock = command->GetMaterial().UniformBlock(Material::InstanceBlockName);
		instanceBlock->GetUniform(Material::TexRectUniformName)->SetFloatVector(texCoords.Data());
		instanceBlock->GetUniform(Material::SpriteSizeUniformName)->SetFloatVector(size.Data());
		instanceBlock->GetUniform(Material::ColorUniformName)->SetFloatVector(finalColor.Data());
		if (indexed) {
			auto* palOffsetUniform = instanceBlock->GetUniform(Material::PaletteOffsetUniformName);
			if (palOffsetUniform != nullptr) {
				palOffsetUniform->SetFloatValue((float)paletteOffset);
			}
		}

		Matrix4x4f worldMatrix = Matrix4x4f::Translation(pos.X, pos.Y, 0.0f);
		if (std::abs(angle) > 0.01f) {
			worldMatrix.Translate(size.X * 0.5f, size.Y * 0.5f, 0.0f);
			worldMatrix.RotateZ(angle);
			worldMatrix.Translate(size.X * -0.5f, size.Y * -0.5f, 0.0f);
		}
		command->SetTransformation(worldMatrix);
		command->SetLayer(z);
		command->GetMaterial().SetTexture(0, texture);
		if (indexed) {
			Texture* palette = ContentResolver::Get().GetPaletteTexture();
			if (palette != nullptr) {
				command->GetMaterial().SetTexture(1, *palette);
			}
		}

		// No live uniform block on PSP - stash the sprite params for the GU emitter (indexed textures are recolored
		// through the flat palette offset; the emitter applies it when baking/CLUT-loading).
		command->SetPspSpriteData(finalColor.Data(), texCoords.X, texCoords.Y, texCoords.Z, texCoords.W,
			size.X, size.Y, (indexed ? (float)paletteOffset : 0.0f));
		command->pspUiSpace_ = true;
		command->SetLayer((std::uint16_t)(command->GetLayer() + RenderCommand::PspUiLayerBase));
		// Ensure the quad geometry is valid even when the shader didn't change (the emitter draws from the stashed
		// params, so guarantee draw params are set); else Issue() drops the command.
		command->GetGeometry().SetDrawParameters(GL_TRIANGLE_STRIP, 0, 4);
		_currentRenderQueue->AddCommand(command);
	}

	void Canvas::DrawTextureWithPalette(const Texture& texture, const Texture& palette, Vector2f pos, std::uint16_t z, Vector2f size, const Vector4f& texCoords, const Colorf& color, float paletteOffset)
	{
#if defined(DEATH_TARGET_PSP)
		{
		(void)palette;
		pos = pos * LayerScale + LayerOffset;
		size = size * LayerScale;
		const Colorf finalColor = color * LayerColor;
		auto command = RentRenderCommand();
		command->GetMaterial().SetBlendingFactors(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		command->SetTransformation(Matrix4x4f::Translation(pos.X, pos.Y, 0.0f));
		command->SetLayer((std::uint16_t)(z + RenderCommand::PspUiLayerBase));
		command->GetMaterial().SetTexture(0, texture);
		command->SetPspSpriteData(finalColor.Data(), texCoords.X, texCoords.Y, texCoords.Z, texCoords.W,
			size.X, size.Y, paletteOffset);
		command->pspUiSpace_ = true;
		command->GetGeometry().SetDrawParameters(GL_TRIANGLE_STRIP, 0, 4);
		_currentRenderQueue->AddCommand(command);
		}
		return;
#endif
		auto* shader = ContentResolver::Get().GetShader(PrecompiledShader::PaletteRemap);
		if (shader == nullptr) {
			// No palette shader - fall back to a plain textured draw (which applies the layer transform itself)
			DrawTexture(texture, pos, z, size, texCoords, color);
			return;
		}

		// Apply the optional per-layer draw transform (menu section transitions; identity by default)
		pos = pos * LayerScale + LayerOffset;
		size = size * LayerScale;
		const Colorf finalColor = color * LayerColor;

		auto command = RentRenderCommand();
		if (command->GetMaterial().SetShader(shader)) {
			command->GetMaterial().ReserveUniformsDataMemory();
			command->GetGeometry().SetDrawParameters(GL_TRIANGLE_STRIP, 0, 4);

			auto* textureUniform = command->GetMaterial().Uniform(Material::TextureUniformName);
			if (textureUniform && textureUniform->GetIntValue(0) != 0) {
				textureUniform->SetIntValue(0); // GL_TEXTURE0
			}
			auto* paletteUniform = command->GetMaterial().Uniform("uTexturePalette");
			if (paletteUniform != nullptr) {
				paletteUniform->SetIntValue(1); // GL_TEXTURE1
			}
		}

		command->GetMaterial().SetBlendingFactors(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

		auto instanceBlock = command->GetMaterial().UniformBlock(Material::InstanceBlockName);
		instanceBlock->GetUniform(Material::TexRectUniformName)->SetFloatVector(texCoords.Data());
		instanceBlock->GetUniform(Material::SpriteSizeUniformName)->SetFloatVector(size.Data());
		instanceBlock->GetUniform(Material::ColorUniformName)->SetFloatVector(finalColor.Data());
		auto* palOffsetUniform = instanceBlock->GetUniform(Material::PaletteOffsetUniformName);
		if (palOffsetUniform != nullptr) {
			palOffsetUniform->SetFloatValue(paletteOffset);
		}

		command->SetTransformation(Matrix4x4f::Translation(pos.X, pos.Y, 0.0f));
		command->SetLayer(z);
		command->GetMaterial().SetTexture(0, texture);
		command->GetMaterial().SetTexture(1, palette);

		command->SetPspSpriteData(finalColor.Data(), texCoords.X, texCoords.Y, texCoords.Z, texCoords.W,
			size.X, size.Y, paletteOffset);
		command->pspUiSpace_ = true;
		command->SetLayer((std::uint16_t)(command->GetLayer() + RenderCommand::PspUiLayerBase));
		// Ensure the quad geometry is valid even when the shader didn't change; else Issue() drops the command.
		command->GetGeometry().SetDrawParameters(GL_TRIANGLE_STRIP, 0, 4);
		_currentRenderQueue->AddCommand(command);
	}

	void Canvas::DrawSolid(Vector2f pos, std::uint16_t z, Vector2f size, const Colorf& color, bool additiveBlending)
	{
		// Apply the optional per-layer draw transform (menu section transitions; identity by default)
		pos = pos * LayerScale + LayerOffset;
		size = size * LayerScale;
		const Colorf finalColor = color * LayerColor;

		auto command = RentRenderCommand();
#if defined(DEATH_TARGET_PSP)
		if (additiveBlending) command->GetMaterial().SetBlendingFactors(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
		else command->GetMaterial().SetBlendingFactors(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		command->SetTransformation(Matrix4x4f::Translation(pos.X, pos.Y, 0.0f));
		command->SetLayer((std::uint16_t)(z + RenderCommand::PspUiLayerBase));
		command->GetMaterial().SetTexture(0, nullptr);
		command->SetPspSpriteData(finalColor.Data(), 1.0f, 0.0f, 1.0f, 0.0f, size.X, size.Y);
		command->pspUiSpace_ = true;
		command->GetGeometry().SetDrawParameters(GL_TRIANGLE_STRIP, 0, 4);
		_currentRenderQueue->AddCommand(command);
		return;
#endif
		if (command->GetMaterial().SetShaderProgramType(Material::ShaderProgramType::SpriteNoTexture)) {
			command->GetMaterial().ReserveUniformsDataMemory();
			command->GetGeometry().SetDrawParameters(GL_TRIANGLE_STRIP, 0, 4);
			// Required to reset render command properly
			//command->SetTransformation(command->transformation());
		}

		if (additiveBlending) {
			command->GetMaterial().SetBlendingFactors(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
		} else {
			command->GetMaterial().SetBlendingFactors(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		}

		auto instanceBlock = command->GetMaterial().UniformBlock(Material::InstanceBlockName);
		instanceBlock->GetUniform(Material::SpriteSizeUniformName)->SetFloatVector(size.Data());
		instanceBlock->GetUniform(Material::ColorUniformName)->SetFloatVector(finalColor.Data());

		command->SetTransformation(Matrix4x4f::Translation(pos.X, pos.Y, 0.0f));
		command->SetLayer(z);
		command->GetMaterial().SetTexture(0, nullptr);

		// Solid (untextured) quad - the emitter draws it as a flat-colored sprite. texRect is unused.
		command->SetPspSpriteData(finalColor.Data(), 1.0f, 0.0f, 1.0f, 0.0f, size.X, size.Y);
		command->pspUiSpace_ = true;
		command->SetLayer((std::uint16_t)(command->GetLayer() + RenderCommand::PspUiLayerBase));
		// Ensure the quad geometry is valid even when the shader didn't change; else Issue() drops the command.
		command->GetGeometry().SetDrawParameters(GL_TRIANGLE_STRIP, 0, 4);
		_currentRenderQueue->AddCommand(command);
	}

	Vector2f Canvas::ApplyAlignment(Alignment align, Vector2f vec, Vector2f size)
	{
		Vector2f result = vec;

		// Sprites are aligned to center by default
		switch (align & Alignment::HorizontalMask) {
			case Alignment::Center: result.X -= size.X * 0.5f; break;
			case Alignment::Right: result.X -= size.X; break;
		}
		switch (align & Alignment::VerticalMask) {
			case Alignment::Center: result.Y -= size.Y * 0.5f; break;
			case Alignment::Bottom: result.Y -= size.Y; break;
		}

		return result;
	}

	RenderCommand* Canvas::RentRenderCommand()
	{
		if (_renderCommandsCount < _renderCommands.size()) {
			RenderCommand* command = _renderCommands[_renderCommandsCount].get();
			command->SetType(RenderCommand::Type::Sprite);
			command->ResetPspSpriteData();
			// A pooled command may still reference a palette texture at unit 1 from a previous frame; that texture
			// can have been freed since (e.g., a menu section's preview palette that was destroyed on navigating
			// back), so clear it to avoid binding a dangling pointer. Palette draws re-bind unit 1 after renting.
			command->GetMaterial().SetTexture(1, nullptr);
			_renderCommandsCount++;
			return command;
		} else {
			std::unique_ptr<RenderCommand>& command = _renderCommands.emplace_back(std::make_unique<RenderCommand>(RenderCommand::Type::Sprite));
			command->GetMaterial().SetBlendingEnabled(true);
			_renderCommandsCount++;
			return command.get();
		}
	}
}
