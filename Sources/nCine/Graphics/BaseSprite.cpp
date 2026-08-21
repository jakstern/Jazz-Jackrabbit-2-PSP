#include "BaseSprite.h"
#include "RenderCommand.h"
#include "../tracy.h"
#if defined(DEATH_TARGET_PSP)
#	include <cstdio>
#endif

namespace nCine
{
	BaseSprite::BaseSprite(SceneNode* parent, Texture* texture, float xx, float yy)
		: DrawableNode(parent, xx, yy), texture_(texture), texRect_(0, 0, 0, 0), flippedX_(false), flippedY_(false), paletteOffset_(0.0f), instanceBlock_(nullptr)
	{
		renderCommand_.GetMaterial().SetBlendingEnabled(true);
	}

	BaseSprite::BaseSprite(SceneNode* parent, Texture* texture, Vector2f position)
		: BaseSprite(parent, texture, position.X, position.Y)
	{
	}

	void BaseSprite::setSize(float width, float height)
	{
		// Update anchor points when size changes
		if (anchorPoint_.X != 0.0f) {
			anchorPoint_.X = (anchorPoint_.X / width_) * width;
		}
		if (anchorPoint_.Y != 0.0f) {
			anchorPoint_.Y = (anchorPoint_.Y / height_) * height;
		}

		width_ = width;
		height_ = height;
		dirtyBits_.set(DirtyBitPositions::SizeBit);
		dirtyBits_.set(DirtyBitPositions::AabbBit);
	}

	/** @note Setting a texture that is already assigned is equivalent to @ref resetTexture() */
	void BaseSprite::setTexture(Texture* texture)
	{
		// Allow self-assignment to take into account the case where the texture stays the same but it loads new data
		textureHasChanged(texture);
		texture_ = texture;
		dirtyBits_.set(DirtyBitPositions::TextureBit);
	}

	/** @note Use this method when the content of the currently assigned texture changes */
	void BaseSprite::resetTexture()
	{
		textureHasChanged(texture_);
		dirtyBits_.set(DirtyBitPositions::TextureBit);
	}

	void BaseSprite::setTexRect(const Recti& rect)
	{
		texRect_ = rect;
		setSize(static_cast<float>(rect.W), static_cast<float>(rect.H));

		if (flippedX_) {
			texRect_.X += texRect_.W;
			texRect_.W *= -1;
		}

		if (flippedY_) {
			texRect_.Y += texRect_.H;
			texRect_.H *= -1;
		}

		dirtyBits_.set(DirtyBitPositions::TextureBit);
	}

	void BaseSprite::setFlippedX(bool flippedX)
	{
		if (flippedX_ != flippedX) {
			texRect_.X += texRect_.W;
			texRect_.W *= -1;
			flippedX_ = flippedX;

			dirtyBits_.set(DirtyBitPositions::TextureBit);
		}
	}

	void BaseSprite::setFlippedY(bool flippedY)
	{
		if (flippedY_ != flippedY) {
			texRect_.Y += texRect_.H;
			texRect_.H *= -1;
			flippedY_ = flippedY;

			dirtyBits_.set(DirtyBitPositions::TextureBit);
		}
	}

	BaseSprite::BaseSprite(const BaseSprite& other)
		: DrawableNode(other), texture_(other.texture_), texRect_(other.texRect_),
			flippedX_(other.flippedX_), flippedY_(other.flippedY_), paletteOffset_(other.paletteOffset_), instanceBlock_(nullptr)
	{
	}

	void BaseSprite::setPaletteOffset(float paletteOffset)
	{
		if (paletteOffset_ != paletteOffset) {
			paletteOffset_ = paletteOffset;
			// Uploaded together with the sprite size (see updateRenderCommand)
			dirtyBits_.set(DirtyBitPositions::SizeBit);
		}
	}

	void BaseSprite::shaderHasChanged()
	{
#if defined(DEATH_TARGET_PSP)
		// PSP has no shader reflection or uniform buffers. The emitter receives sprite state directly from the
		// RenderCommand, so creating and looking up an InstanceBlock here is dead per-sprite setup.
		instanceBlock_ = nullptr;
#else
		renderCommand_.GetMaterial().ReserveUniformsDataMemory();
		instanceBlock_ = renderCommand_.GetMaterial().UniformBlock(Material::InstanceBlockName);
		GLUniformCache* textureUniform = renderCommand_.GetMaterial().Uniform(Material::TextureUniformName);
		if (textureUniform != nullptr && textureUniform->GetIntValue(0) != 0) {
			textureUniform->SetIntValue(0); // GL_TEXTURE0
		}
#endif

		dirtyBits_.set(DirtyBitPositions::TransformationBit);
		dirtyBits_.set(DirtyBitPositions::ColorBit);
		dirtyBits_.set(DirtyBitPositions::SizeBit);
		dirtyBits_.set(DirtyBitPositions::TextureBit);
	}

	void BaseSprite::updateRenderCommand()
	{
		ZoneScopedC(0x81A861);

#if defined(DEATH_TARGET_PSP)
		if (dirtyBits_.test(DirtyBitPositions::TransformationUploadBit)) {
			renderCommand_.SetTransformation(worldMatrix_);
		}
		if (dirtyBits_.test(DirtyBitPositions::TextureBit)) {
			if (texture_ != nullptr) renderCommand_.GetMaterial().SetTexture(*texture_);
			else renderCommand_.GetMaterial().SetTexture(nullptr);
			dirtyBits_.reset(DirtyBitPositions::TextureBit);
		}

		float sx = 1.0f, bx = 0.0f, sy = 1.0f, by = 0.0f;
		if (texture_ != nullptr) {
			const Vector2i texSize = texture_->GetSize();
			if (texSize.X > 0 && texSize.Y > 0) {
				sx = texRect_.W / float(texSize.X);
				bx = texRect_.X / float(texSize.X);
				sy = texRect_.H / float(texSize.Y);
				by = texRect_.Y / float(texSize.Y);
			}
		}
		renderCommand_.SetPspSpriteData(absColor().Data(), sx, bx, sy, by, float(width_), float(height_), paletteOffset_);
		dirtyBits_.reset(DirtyBitPositions::SizeBit);
		return;
#else
		if (dirtyBits_.test(DirtyBitPositions::TransformationUploadBit)) {
			renderCommand_.SetTransformation(worldMatrix_);
			//dirtyBits_.reset(DirtyBitPositions::TransformationUploadBit);
		}
		if (dirtyBits_.test(DirtyBitPositions::ColorUploadBit)) {
			GLUniformCache* colorUniform = instanceBlock_->GetUniform(Material::ColorUniformName);
			if (colorUniform != nullptr) {
				colorUniform->SetFloatVector(absColor().Data());
			}
			//dirtyBits_.reset(DirtyBitPositions::ColorUploadBit);
		}
		if (dirtyBits_.test(DirtyBitPositions::SizeBit)) {
			GLUniformCache* spriteSizeUniform = instanceBlock_->GetUniform(Material::SpriteSizeUniformName);
			if (spriteSizeUniform != nullptr) {
				spriteSizeUniform->SetFloatValue(width_, height_);
			}
			// Present only in palette shaders (sprite_vs/batched_sprites_vs); null elsewhere
			GLUniformCache* palOffsetUniform = instanceBlock_->GetUniform(Material::PaletteOffsetUniformName);
			if (palOffsetUniform != nullptr) {
				palOffsetUniform->SetFloatValue(paletteOffset_);
			}
			dirtyBits_.reset(DirtyBitPositions::SizeBit);
		}

		if (dirtyBits_.test(DirtyBitPositions::TextureBit)) {
			if (texture_ != nullptr) {
				renderCommand_.GetMaterial().SetTexture(*texture_);

				GLUniformCache* texRectUniform = instanceBlock_->GetUniform(Material::TexRectUniformName);
				if (texRectUniform != nullptr) {
					const Vector2i texSize = texture_->GetSize();
					const float texScaleX = texRect_.W / float(texSize.X);
					const float texBiasX = texRect_.X / float(texSize.X);
					const float texScaleY = texRect_.H / float(texSize.Y);
					const float texBiasY = texRect_.Y / float(texSize.Y);

					texRectUniform->SetFloatValue(texScaleX, texBiasX, texScaleY, texBiasY);
				}
			} else {
				renderCommand_.GetMaterial().SetTexture(nullptr);
			}

			dirtyBits_.reset(DirtyBitPositions::TextureBit);
		}

#if defined(DEATH_TARGET_PSP)
		// On PSP the InstanceBlock UBO has no reflected uniforms (no GL), so stash the sprite parameters
		// directly on the render command for the GU emitter. The model matrix is already set via
		// SetTransformation above; texRect matches the shader's (scaleX, biasX, scaleY, biasY) layout.
		{
			float sx = 1.0f, bx = 0.0f, sy = 1.0f, by = 0.0f;
			if (texture_ != nullptr) {
				const Vector2i texSize = texture_->GetSize();
				if (texSize.X > 0 && texSize.Y > 0) {
					sx = texRect_.W / float(texSize.X);
					bx = texRect_.X / float(texSize.X);
					sy = texRect_.H / float(texSize.Y);
					by = texRect_.Y / float(texSize.Y);
				}
			}
			renderCommand_.SetPspSpriteData(absColor().Data(), sx, bx, sy, by, float(width_), float(height_), paletteOffset_);
		}
#endif
#endif
	}
}
