/*
 * Copyright (C) 2021 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/reshade#license
 */

#include "render_d3d9.hpp"

using namespace reshade::api;

static void convert_usage_to_d3d_usage(const resource_usage usage, DWORD &d3d_usage)
{
	// Copying textures is implemented using the rasterization pipeline (see 'device_impl::copy_resource' implementation), so needs render target usage
	// When the destination in 'IDirect3DDevice9::StretchRect' is a texture surface, it too has to have render target usage (see https://docs.microsoft.com/windows/win32/api/d3d9helper/nf-d3d9helper-idirect3ddevice9-stretchrect)
	if ((usage & (resource_usage::render_target | resource_usage::copy_dest | resource_usage::resolve_dest)) != 0)
		d3d_usage |= D3DUSAGE_RENDERTARGET;
	else
		d3d_usage &= ~D3DUSAGE_RENDERTARGET;

	if ((usage & (resource_usage::depth_stencil)) != 0)
		d3d_usage |= D3DUSAGE_DEPTHSTENCIL;
	else
		d3d_usage &= ~D3DUSAGE_DEPTHSTENCIL;

	// Unordered access is not supported in D3D9
	assert((usage & resource_usage::unordered_access) == 0);
}
static void convert_d3d_usage_to_usage(const DWORD d3d_usage, resource_usage &usage)
{
	if (d3d_usage & D3DUSAGE_RENDERTARGET)
		usage |= resource_usage::render_target;
	if (d3d_usage & D3DUSAGE_DEPTHSTENCIL)
		usage |= resource_usage::depth_stencil;
}

void reshade::d3d9::convert_resource_desc(const resource_desc &desc, D3DVOLUME_DESC &internal_desc, UINT *levels)
{
	internal_desc.Width = desc.width;
	internal_desc.Height = desc.height;
	internal_desc.Depth = desc.depth_or_layers;
	internal_desc.Format = static_cast<D3DFORMAT>(desc.format);
	assert(desc.samples == 1);

	convert_usage_to_d3d_usage(desc.usage, internal_desc.Usage);

	if (levels != nullptr)
		*levels = desc.levels;
	else
		assert(desc.levels == 1);
}
void reshade::d3d9::convert_resource_desc(const resource_desc &desc, D3DSURFACE_DESC &internal_desc, UINT *levels)
{
	internal_desc.Width = desc.width;
	internal_desc.Height = desc.height;
	assert(desc.depth_or_layers == 1 || desc.depth_or_layers == 6 /* D3DRTYPE_CUBETEXTURE */);
	internal_desc.Format = static_cast<D3DFORMAT>(desc.format);

	if (desc.samples > 1)
		internal_desc.MultiSampleType = static_cast<D3DMULTISAMPLE_TYPE>(desc.samples);
	else
		internal_desc.MultiSampleType = D3DMULTISAMPLE_NONE;

	convert_usage_to_d3d_usage(desc.usage, internal_desc.Usage);

	if (levels != nullptr)
		*levels = desc.levels;
	else
		assert(desc.levels == 1);
}
void reshade::d3d9::convert_resource_desc(const resource_desc &desc, D3DINDEXBUFFER_DESC &internal_desc)
{
	internal_desc.Size = desc.width;
	assert(desc.height == 0 && desc.depth_or_layers == 0 && desc.levels == 0 && desc.format == 0 && desc.samples == 0);
	assert((desc.usage & (resource_usage::vertex_buffer | resource_usage::index_buffer)) == resource_usage::index_buffer);
	convert_usage_to_d3d_usage(desc.usage, internal_desc.Usage);
}
void reshade::d3d9::convert_resource_desc(const resource_desc &desc, D3DVERTEXBUFFER_DESC &internal_desc)
{
	internal_desc.Size = desc.width;
	assert(desc.height == 0 && desc.depth_or_layers == 0 && desc.levels == 0 && desc.format == 0 && desc.samples == 0);
	assert((desc.usage & (resource_usage::vertex_buffer | resource_usage::index_buffer)) == resource_usage::vertex_buffer);
	convert_usage_to_d3d_usage(desc.usage, internal_desc.Usage);
}
resource_desc reshade::d3d9::convert_resource_desc(const D3DVOLUME_DESC &internal_desc, UINT levels)
{
	assert(internal_desc.Type == D3DRTYPE_VOLUME || internal_desc.Type == D3DRTYPE_VOLUMETEXTURE);

	resource_desc desc = {};
	desc.width = internal_desc.Width;
	desc.height = internal_desc.Height;
	assert(internal_desc.Depth <= std::numeric_limits<uint16_t>::max());
	desc.depth_or_layers = static_cast<uint16_t>(internal_desc.Depth);
	assert(levels <= std::numeric_limits<uint16_t>::max());
	desc.levels = static_cast<uint16_t>(levels);
	desc.format = static_cast<uint32_t>(internal_desc.Format);
	desc.samples = 1;

	convert_d3d_usage_to_usage(internal_desc.Usage, desc.usage);
	if (internal_desc.Type == D3DRTYPE_VOLUMETEXTURE)
		desc.usage |= resource_usage::shader_resource;

	return desc;
}
resource_desc reshade::d3d9::convert_resource_desc(const D3DSURFACE_DESC &internal_desc, UINT levels, const D3DCAPS9 &caps)
{
	assert(internal_desc.Type == D3DRTYPE_SURFACE || internal_desc.Type == D3DRTYPE_TEXTURE || internal_desc.Type == D3DRTYPE_CUBETEXTURE);

	resource_desc desc = {};
	desc.width = internal_desc.Width;
	desc.height = internal_desc.Height;
	desc.depth_or_layers = internal_desc.Type == D3DRTYPE_CUBETEXTURE ? 6 : 1;
	assert(levels <= std::numeric_limits<uint16_t>::max());
	desc.levels = static_cast<uint16_t>(levels);
	desc.format = static_cast<uint32_t>(internal_desc.Format);

	if (internal_desc.MultiSampleType >= D3DMULTISAMPLE_2_SAMPLES)
		desc.samples = static_cast<uint16_t>(internal_desc.MultiSampleType);
	else
		desc.samples = 1;

	convert_d3d_usage_to_usage(internal_desc.Usage, desc.usage);
	if (internal_desc.Type == D3DRTYPE_TEXTURE || internal_desc.Type == D3DRTYPE_CUBETEXTURE)
		desc.usage |= resource_usage::shader_resource;

	// Copying is restricted by limitations of 'IDirect3DDevice9::StretchRect' (see https://docs.microsoft.com/windows/win32/api/d3d9helper/nf-d3d9helper-idirect3ddevice9-stretchrect)
	// or performing copy between two textures using rasterization pipeline (see 'device_impl::copy_resource' implementation)
	if (internal_desc.Pool == D3DPOOL_DEFAULT && (internal_desc.Type == D3DRTYPE_SURFACE || (internal_desc.Type == D3DRTYPE_TEXTURE && (caps.Caps2 & D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES) != 0)))
	{
		switch (static_cast<DWORD>(internal_desc.Format))
		{
		default:
			desc.usage |= resource_usage::copy_source;
			if (internal_desc.MultiSampleType >= D3DMULTISAMPLE_2_SAMPLES)
				desc.usage |= resource_usage::resolve_source;
			if (internal_desc.Usage & D3DUSAGE_RENDERTARGET)
				desc.usage |= resource_usage::copy_dest | resource_usage::resolve_dest;
			break;
		case D3DFMT_DXT1:
		case D3DFMT_DXT2:
		case D3DFMT_DXT3:
		case D3DFMT_DXT4:
		case D3DFMT_DXT5:
			// Stretching is not supported if either surface is in a compressed format
			break;
		case D3DFMT_D16_LOCKABLE:
		case D3DFMT_D32:
		case D3DFMT_D15S1:
		case D3DFMT_D24S8:
		case D3DFMT_D24X8:
		case D3DFMT_D24X4S4:
		case D3DFMT_D16:
		case D3DFMT_D32F_LOCKABLE:
		case D3DFMT_D24FS8:
		case D3DFMT_D32_LOCKABLE:
		case D3DFMT_S8_LOCKABLE:
			// Stretching depth stencil surfaces is extremly limited (does not support copying from surface to texture for example), so just do not allow it
			assert(internal_desc.Usage & D3DUSAGE_DEPTHSTENCIL);
			break;
		case MAKEFOURCC('N', 'U', 'L', 'L'):
			// Special render target format that has no memory attached, so cannot be copied
			break;
		}
	}

	return desc;
}
resource_desc reshade::d3d9::convert_resource_desc(const D3DINDEXBUFFER_DESC &internal_desc)
{
	resource_desc desc = {};
	desc.width = internal_desc.Size;
	convert_d3d_usage_to_usage(internal_desc.Usage, desc.usage);
	desc.usage |= resource_usage::index_buffer;
	return desc;
}
resource_desc reshade::d3d9::convert_resource_desc(const D3DVERTEXBUFFER_DESC &internal_desc)
{
	resource_desc desc = {};
	desc.width = internal_desc.Size;
	convert_d3d_usage_to_usage(internal_desc.Usage, desc.usage);
	desc.usage |= resource_usage::vertex_buffer;
	return desc;
}

reshade::d3d9::device_impl::device_impl(IDirect3DDevice9 *device) :
	api_object_impl(device), _caps(), _cp(), _backup_state(device)
{
	_orig->GetDirect3D(&_d3d);
	_orig->GetDeviceCaps(&_caps);
	_orig->GetCreationParameters(&_cp);

	// Limit maximum simultaneous number of render targets to 8 (usually only 4 in D3D9 anyway)
	if (_caps.NumSimultaneousRTs > 8)
		_caps.NumSimultaneousRTs = 8;

#if RESHADE_ADDON
	reshade::addon::load_addons();
#endif

	com_ptr<IDirect3DSwapChain9> swapchain;
	device->GetSwapChain(0, &swapchain);
	assert(swapchain != nullptr); // There should always be an implicit swap chain

	D3DPRESENT_PARAMETERS pp = {};
	swapchain->GetPresentParameters(&pp);
	on_after_reset(pp);
}
reshade::d3d9::device_impl::~device_impl()
{
	on_reset();

#if RESHADE_ADDON
	reshade::addon::unload_addons();
#endif
}

void reshade::d3d9::device_impl::on_reset()
{
	// Do not call add-on events if this device was already reset before
	if (_copy_state == nullptr)
		return;

	// Force add-ons to release all resources associated with this device before performing reset
	RESHADE_ADDON_EVENT(destroy_command_queue, this);
	RESHADE_ADDON_EVENT(destroy_device, this);

	_copy_state.reset();
	_backup_state.release_state_block();
}
void reshade::d3d9::device_impl::on_after_reset(const D3DPRESENT_PARAMETERS &pp)
{
	// Create state blocks used for resource copying
	HRESULT hr = _orig->BeginStateBlock();
	if (SUCCEEDED(hr))
	{
		_orig->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
		_orig->SetPixelShader(nullptr);
		_orig->SetVertexShader(nullptr);
		_orig->SetRenderState(D3DRS_ZENABLE, false);
		_orig->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
		_orig->SetRenderState(D3DRS_ALPHATESTENABLE, false);
		_orig->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
		_orig->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
		_orig->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		_orig->SetRenderState(D3DRS_ALPHABLENDENABLE, false);
		_orig->SetRenderState(D3DRS_FOGENABLE, false);
		_orig->SetRenderState(D3DRS_STENCILENABLE, false);
		_orig->SetRenderState(D3DRS_CLIPPING, false);
		_orig->SetRenderState(D3DRS_LIGHTING, false);
		_orig->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
		_orig->SetRenderState(D3DRS_SCISSORTESTENABLE, false);
		_orig->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
		_orig->SetRenderState(D3DRS_SRGBWRITEENABLE, false);
		_orig->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
		_orig->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		_orig->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		_orig->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		_orig->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		_orig->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
		_orig->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		_orig->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		_orig->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		_orig->SetSamplerState(0, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
		_orig->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		_orig->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		_orig->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		_orig->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, 0);
		_orig->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
		_orig->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 0);

		hr = _orig->EndStateBlock(&_copy_state);
		if (FAILED(hr))
			return;

		if (!_backup_state.init_state_block())
			return;
	}

	RESHADE_ADDON_EVENT(init_device, this);
	RESHADE_ADDON_EVENT(init_command_queue, this);

#if RESHADE_ADDON
	if (com_ptr<IDirect3DSurface9> auto_depth_stencil;
		pp.EnableAutoDepthStencil &&
		SUCCEEDED(_orig->GetDepthStencilSurface(&auto_depth_stencil)))
	{
		D3DSURFACE_DESC desc = {};
		auto_depth_stencil->GetDesc(&desc);
		D3DSURFACE_DESC new_desc = desc;

		reshade::api::resource_desc api_desc = convert_resource_desc(desc, 1, _caps);
		RESHADE_ADDON_EVENT(create_resource, this, resource_type::surface, &api_desc);
		convert_resource_desc(api_desc, new_desc);

		// Need to replace auto depth stencil if add-on modified the description
		if (com_ptr<IDirect3DSurface9> auto_depth_stencil_replacement;
			std::memcmp(&desc, &new_desc, sizeof(D3DSURFACE_DESC)) != 0 &&
			create_surface_replacement(new_desc, &auto_depth_stencil_replacement))
		{
			// The device will hold a reference to the surface after binding it, so can release this one afterwards
			_orig->SetDepthStencilSurface(auto_depth_stencil_replacement.get());

			auto_depth_stencil = std::move(auto_depth_stencil_replacement);
		}
		else
		{
			_resources.register_object(auto_depth_stencil.get());
		}

		// Communicate default state to add-ons
		const reshade::api::resource_view_handle dsv = { reinterpret_cast<uintptr_t>(auto_depth_stencil.get()) };
		RESHADE_ADDON_EVENT(set_render_targets_and_depth_stencil, this, 0, nullptr, dsv);
	}
#endif
}

bool reshade::d3d9::device_impl::create_surface_replacement(const D3DSURFACE_DESC &new_desc, IDirect3DSurface9 **out_surface, HANDLE *out_shared_handle)
{
	com_ptr<IDirect3DTexture9> texture; // Surface will hold a reference to the created texture and keep it alive
	if (new_desc.MultiSampleType == D3DMULTISAMPLE_NONE &&
		SUCCEEDED(_orig->CreateTexture(new_desc.Width, new_desc.Height, 1, new_desc.Usage, new_desc.Format, new_desc.Pool, &texture, out_shared_handle)) &&
		SUCCEEDED(texture->GetSurfaceLevel(0, out_surface)))
	{
		_resources.register_object(texture.get());
		_resources.register_object(*out_surface);
		return true; // Successfully created replacement texture and got surface to it
	}
	return false;
}

bool reshade::d3d9::device_impl::check_format_support(uint32_t format, resource_usage usage) const
{
	if ((usage & resource_usage::unordered_access) != 0)
		return false;

	DWORD d3d_usage = 0;
	convert_usage_to_d3d_usage(usage, d3d_usage);

	return SUCCEEDED(_d3d->CheckDeviceFormat(_cp.AdapterOrdinal, _cp.DeviceType, D3DFMT_X8R8G8B8, d3d_usage, D3DRTYPE_TEXTURE, static_cast<D3DFORMAT>(format)));
}

bool reshade::d3d9::device_impl::check_resource_handle_valid(resource_handle resource) const
{
	return resource.handle != 0 && _resources.has_object(reinterpret_cast<IDirect3DResource9 *>(resource.handle));
}
bool reshade::d3d9::device_impl::check_resource_view_handle_valid(resource_view_handle view) const
{
	return check_resource_handle_valid({ view.handle });
}

bool reshade::d3d9::device_impl::create_resource(resource_type type, const resource_desc &desc, resource_usage, resource_handle *out_resource)
{
	switch (type)
	{
		case resource_type::buffer:
		{
			if (desc.usage == resource_usage::index_buffer)
			{
				D3DINDEXBUFFER_DESC internal_desc = {};
				convert_resource_desc(desc, internal_desc);

				// TODO: Index format
				if (IDirect3DIndexBuffer9 *resource;
					SUCCEEDED(_orig->CreateIndexBuffer(internal_desc.Size, internal_desc.Usage, D3DFMT_UNKNOWN, D3DPOOL_DEFAULT, &resource, nullptr)))
				{
					_resources.register_object(resource);
					*out_resource = { reinterpret_cast<uintptr_t>(resource) };
					return true;
				}
			}
			if (desc.usage == resource_usage::vertex_buffer)
			{
				D3DVERTEXBUFFER_DESC internal_desc = {};
				convert_resource_desc(desc, internal_desc);

				if (IDirect3DVertexBuffer9 *resource;
					SUCCEEDED(_orig->CreateVertexBuffer(internal_desc.Size, internal_desc.Usage, 0, D3DPOOL_DEFAULT, &resource, nullptr)))
				{
					_resources.register_object(resource);
					*out_resource = { reinterpret_cast<uintptr_t>(resource) };
					return true;
				}
			}
			break;
		}
		case resource_type::texture_1d:
		case resource_type::texture_2d:
		{
			// Array or multisample textures are not supported in Direct3D 9
			if (desc.depth_or_layers != 1 || desc.samples != 1)
				break;

			UINT levels = 0;
			D3DSURFACE_DESC internal_desc = {};
			convert_resource_desc(desc, internal_desc, &levels);

			if (IDirect3DTexture9 *resource;
				SUCCEEDED(_orig->CreateTexture(internal_desc.Width, internal_desc.Height, levels, internal_desc.Usage, internal_desc.Format, D3DPOOL_DEFAULT, &resource, nullptr)))
			{
				_resources.register_object(resource);
				*out_resource = { reinterpret_cast<uintptr_t>(resource) };
				return true;
			}
			break;
		}
		case resource_type::texture_3d:
		{
			// 3D textures can never have multisampling
			if (desc.samples != 1)
				break;

			UINT levels = 0;
			D3DVOLUME_DESC internal_desc = {};
			convert_resource_desc(desc, internal_desc, &levels);

			if (IDirect3DVolumeTexture9 *resource;
				SUCCEEDED(_orig->CreateVolumeTexture(internal_desc.Width, internal_desc.Height, internal_desc.Depth, levels, internal_desc.Usage, internal_desc.Format, D3DPOOL_DEFAULT, &resource, nullptr)))
			{
				_resources.register_object(resource);
				*out_resource = { reinterpret_cast<uintptr_t>(resource) };
				return true;
			}
			break;
		}
	}

	*out_resource = { 0 };
	return false;
}
bool reshade::d3d9::device_impl::create_resource_view(resource_handle resource, resource_view_type type, const resource_view_desc &desc, resource_view_handle *out_view)
{
	assert(resource.handle != 0);
	auto resource_object = reinterpret_cast<IDirect3DResource9 *>(resource.handle);

	// Views with a different format than the resource are not supported in Direct3D 9
	assert(desc.format == get_resource_desc(resource).format);

	switch (resource_object->GetType())
	{
		case D3DRTYPE_SURFACE:
		{
			assert(desc.dimension == resource_view_dimension::texture_2d || desc.dimension == resource_view_dimension::texture_2d_multisample);
			assert(desc.first_layer == 0 && (desc.layers == 1 || desc.layers == std::numeric_limits<uint32_t>::max()));

			if (type == resource_view_type::depth_stencil || type == resource_view_type::render_target)
			{
				if (desc.first_level != 0 || desc.levels != 1)
					break;

				resource_object->AddRef();
				*out_view = { resource.handle };
				return true;
			}
			break;
		}
		case D3DRTYPE_TEXTURE:
		{
			assert(desc.dimension == resource_view_dimension::texture_2d || desc.dimension == resource_view_dimension::texture_2d_multisample);
			assert(desc.first_layer == 0 && (desc.layers == 1 || desc.layers == std::numeric_limits<uint32_t>::max()));

			if (type == resource_view_type::depth_stencil || type == resource_view_type::render_target)
			{
				if (desc.levels != 1)
					break;

				if (IDirect3DSurface9 *surface = nullptr;
					SUCCEEDED(static_cast<IDirect3DTexture9 *>(resource_object)->GetSurfaceLevel(desc.first_level, &surface)))
				{
					*out_view = { reinterpret_cast<uintptr_t>(surface) };
					return true;
				}
			}
			else if (type == resource_view_type::shader_resource && desc.first_level == 0)
			{
				resource_object->AddRef();
				*out_view = { resource.handle };
				return true;
			}
			break;
		}
		case D3DRTYPE_CUBETEXTURE:
		{
			if (type == resource_view_type::depth_stencil || type == resource_view_type::render_target)
			{
				assert(desc.dimension == resource_view_dimension::texture_2d || desc.dimension == resource_view_dimension::texture_2d_multisample);

				if (desc.levels != 1 || desc.layers != 1)
					break;

				if (IDirect3DSurface9 *surface = nullptr;
					SUCCEEDED(static_cast<IDirect3DCubeTexture9 *>(resource_object)->GetCubeMapSurface(static_cast<D3DCUBEMAP_FACES>(desc.first_layer), desc.first_level, &surface)))
				{
					*out_view = { reinterpret_cast<uintptr_t>(surface) };
					return true;
				}
			}
			else if (type == resource_view_type::shader_resource && desc.first_level == 0 && desc.first_layer == 0)
			{
				assert(desc.dimension == resource_view_dimension::texture_cube);

				resource_object->AddRef();
				*out_view = { resource.handle };
				return true;
			}
			break;
		}
	}

	*out_view = { 0 };
	return false;
}

void reshade::d3d9::device_impl::destroy_resource(resource_handle resource)
{
	assert(resource.handle != 0);
	reinterpret_cast<IDirect3DResource9 *>(resource.handle)->Release();
}
void reshade::d3d9::device_impl::destroy_resource_view(resource_view_handle view)
{
	destroy_resource({ view.handle });
}

void reshade::d3d9::device_impl::get_resource_from_view(resource_view_handle view, resource_handle *out_resource) const
{
	assert(view.handle != 0);
	auto resource_object = reinterpret_cast<IDirect3DResource9 *>(view.handle);

	if (com_ptr<IDirect3DSurface9> surface;
		SUCCEEDED(resource_object->QueryInterface(IID_PPV_ARGS(&surface))))
	{
		if (com_ptr<IDirect3DResource9> resource;
			SUCCEEDED(surface->GetContainer(IID_PPV_ARGS(&resource))))
		{
			*out_resource = { reinterpret_cast<uintptr_t>(resource.get()) };
			return;
		}
	}

	// If unable to get container, just return the resource directly
	*out_resource = { view.handle };
}

resource_desc reshade::d3d9::device_impl::get_resource_desc(resource_handle resource) const
{
	assert(resource.handle != 0);
	auto resource_object = reinterpret_cast<IDirect3DResource9 *>(resource.handle);

	switch (resource_object->GetType())
	{
		case D3DRTYPE_SURFACE:
		{
			D3DSURFACE_DESC internal_desc = {};
			static_cast<IDirect3DSurface9 *>(resource_object)->GetDesc(&internal_desc);
			return convert_resource_desc(internal_desc, 1, _caps);
		}
		case D3DRTYPE_TEXTURE:
		{
			D3DSURFACE_DESC internal_desc = {};
			static_cast<IDirect3DTexture9 *>(resource_object)->GetLevelDesc(0, &internal_desc);
			internal_desc.Type = D3DRTYPE_TEXTURE;
			return convert_resource_desc(internal_desc, static_cast<IDirect3DTexture9 *>(resource_object)->GetLevelCount(), _caps);
		}
		case D3DRTYPE_VOLUMETEXTURE:
		{
			D3DVOLUME_DESC internal_desc = {};
			static_cast<IDirect3DVolumeTexture9 *>(resource_object)->GetLevelDesc(0, &internal_desc);
			internal_desc.Type = D3DRTYPE_VOLUMETEXTURE;
			return convert_resource_desc(internal_desc, static_cast<IDirect3DVolumeTexture9 *>(resource_object)->GetLevelCount());
		}
		case D3DRTYPE_CUBETEXTURE:
		{
			D3DSURFACE_DESC internal_desc = {};
			static_cast<IDirect3DCubeTexture9 *>(resource_object)->GetLevelDesc(0, &internal_desc);
			internal_desc.Type = D3DRTYPE_CUBETEXTURE;
			return convert_resource_desc(internal_desc, static_cast<IDirect3DCubeTexture9 *>(resource_object)->GetLevelCount(), _caps);
		}
		case D3DRTYPE_VERTEXBUFFER:
		{
			D3DVERTEXBUFFER_DESC internal_desc = {};
			static_cast<IDirect3DVertexBuffer9 *>(resource_object)->GetDesc(&internal_desc);
			return convert_resource_desc(internal_desc);
		}
		case D3DRTYPE_INDEXBUFFER:
		{
			D3DINDEXBUFFER_DESC internal_desc = {};
			static_cast<IDirect3DIndexBuffer9 *>(resource_object)->GetDesc(&internal_desc);
			return convert_resource_desc(internal_desc);
		}
	}

	assert(false); // Not implemented
	return {};
}

void reshade::d3d9::device_impl::draw(uint32_t vertices, uint32_t instances, uint32_t first_vertex, uint32_t first_instance)
{
	assert(instances <= 1 && first_instance == 0);
	_orig->DrawPrimitive(D3DPT_TRIANGLELIST, first_vertex, vertices / 3);
}
void reshade::d3d9::device_impl::draw_indexed(uint32_t indices, uint32_t instances, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance)
{
	assert(instances <= 1 && first_instance == 0);
	_orig->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, vertex_offset, 0, indices, first_index, indices / 3);
}

void reshade::d3d9::device_impl::copy_resource(resource_handle source, resource_handle destination)
{
	assert(source.handle != 0 && destination.handle != 0);
	const auto source_object = reinterpret_cast<IDirect3DResource9 *>(source.handle);
	const auto destination_object = reinterpret_cast<IDirect3DResource9 *>(destination.handle);

	switch (source_object->GetType() | (destination_object->GetType() << 4))
	{
		case D3DRTYPE_SURFACE | (D3DRTYPE_SURFACE << 4):
		{
			_orig->StretchRect(static_cast<IDirect3DSurface9 *>(source_object), nullptr, static_cast<IDirect3DSurface9 *>(destination_object), nullptr, D3DTEXF_NONE);
			return;
		}
		case D3DRTYPE_SURFACE | (D3DRTYPE_TEXTURE << 4):
		{
			com_ptr<IDirect3DSurface9> destination_surface;
			static_cast<IDirect3DTexture9 *>(destination_object)->GetSurfaceLevel(0, &destination_surface);
			_orig->StretchRect(static_cast<IDirect3DSurface9 *>(source_object), nullptr, destination_surface.get(), nullptr, D3DTEXF_NONE);
			return;
		}
		case D3DRTYPE_TEXTURE | (D3DRTYPE_TEXTURE << 4):
		{
			// Capture and restore state, render targets, depth stencil surface and viewport (which all may change next)
			_backup_state.capture();

			// Perform copy using rasterization pipeline
			_copy_state->Apply();

			// TODO: This copies the first mipmap level only ...
			com_ptr<IDirect3DSurface9> destination_surface;
			static_cast<IDirect3DTexture9 *>(destination_object)->GetSurfaceLevel(0, &destination_surface);
			_orig->SetTexture(0, static_cast<IDirect3DTexture9 *>(source_object));
			_orig->SetRenderTarget(0, destination_surface.get());
			for (DWORD target = 1; target < _caps.NumSimultaneousRTs; ++target)
				_orig->SetRenderTarget(target, nullptr);
			_orig->SetDepthStencilSurface(nullptr);

			const float vertices[4][5] = {
				// x      y      z      tu     tv
				{ -1.0f,  1.0f,  0.0f,  0.0f,  0.0f },
				{  1.0f,  1.0f,  0.0f,  1.0f,  0.0f },
				{ -1.0f, -1.0f,  0.0f,  0.0f,  1.0f },
				{  1.0f, -1.0f,  0.0f,  1.0f,  1.0f },
			};
			_orig->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(vertices[0]));

			_backup_state.apply_and_release();
			return;
		}
		case D3DRTYPE_TEXTURE | (D3DRTYPE_SURFACE << 4):
		{
			com_ptr<IDirect3DSurface9> source_surface;
			static_cast<IDirect3DTexture9 *>(source_object)->GetSurfaceLevel(0, &source_surface);
			_orig->StretchRect(source_surface.get(), nullptr, static_cast<IDirect3DSurface9 *>(destination_object), nullptr, D3DTEXF_NONE);
			return;
		}
	}

	assert(false); // Not implemented
}

void reshade::d3d9::device_impl::clear_depth_stencil_view(resource_view_handle dsv, uint32_t clear_flags, float depth, uint8_t stencil)
{
	_backup_state.capture();

	_orig->SetDepthStencilSurface(reinterpret_cast<IDirect3DSurface9 *>(dsv.handle));

	_orig->Clear(
		0, nullptr,
		((clear_flags & 0x1) != 0 ? D3DCLEAR_ZBUFFER : 0) |
		((clear_flags & 0x2) != 0 ? D3DCLEAR_STENCIL : 0),
		0, depth, stencil);

	_backup_state.apply_and_release();
}
void reshade::d3d9::device_impl::clear_render_target_view(resource_view_handle rtv, const float color[4])
{
	_backup_state.capture();

	_orig->SetRenderTarget(0, reinterpret_cast<IDirect3DSurface9 *>(rtv.handle));
	for (DWORD target = 1; target < _caps.NumSimultaneousRTs; ++target)
		_orig->SetRenderTarget(target, nullptr);

	_orig->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_COLORVALUE(color[0], color[1], color[2], color[3]), 0.0f, 0);

	_backup_state.apply_and_release();
}