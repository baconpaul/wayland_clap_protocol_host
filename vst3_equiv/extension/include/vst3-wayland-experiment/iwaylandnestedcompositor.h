#pragma once

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/gui/iplugview.h"

#include "pluginterfaces/base/falignpush.h"

/// @page VST3 Wayland Experiment
///
/// The VST3 equivalent of clap-wayland-experiment's nested compositor
/// (CLAP_WINDOW_API_WAYLAND_SURFACE_EMBEDDING_V1 and clap.gui-wayland-nested-compositor): the
/// plugin's view is embedded into the host's window through a Wayland compositor which the host
/// runs, with the clap_surface_embedding_v1 protocol. The protocol is plain Wayland, nothing in it
/// is specific to CLAP but its name: it is in protocols/clap-surface-embedding-v1.xml of the
/// clap-wayland-experiment repository.
///
/// It is an alternative to VST 3.8's kPlatformTypeWaylandSurfaceID, IWaylandHost and
/// IWaylandFrame, which pass wl_display, wl_surface, xdg_surface and xdg_toplevel pointers between
/// the host and the plugin. Those pointers tie both to one libwayland-client in one process: they
/// must load the same libwayland, and the plugin can't use another Wayland implementation, or run
/// its GUI in another process. Here only a socket and strings go across:
/// - IWaylandNestedCompositor::openConnection() gives the plugin a socket to the host's compositor,
///   which the plugin connects on with its own Wayland library
/// - IPlugView::attached() gives the plugin an embedding token, which it passes to
///   clap_surface_embedding_v1.get_embedded_surface()
/// - for its dialogs and popups, the plugin uses xdg-shell on the same connection, with the parents
///   from clap_embedded_surface_v1.get_parent_toplevel() and get_parent_surface()
///
/// Opening the view works as follows:
///  1. the host queries the IHostApplication context which it gave to IPluginBase::initialize()
///     for IWaylandNestedCompositor, the plugin may do it from initialize() on
///  2. IEditController::createView (ViewType::kEditor)
///  3. IPlugView::isPlatformTypeSupported (kPlatformTypeWaylandSurfaceEmbeddingV1)
///  4. IPlugView::setFrame (frame), whose IPlugFrame also implements Linux::IRunLoop
///  5. IPlugView::getSize (), and the host makes room for the view
///  6. IPlugView::attached (token, kPlatformTypeWaylandSurfaceEmbeddingV1)
/// and IPlugView::removed () ends the embedding.
///
/// The plugin's connection is a Wayland client like any other, so the plugin reads its events
/// when the connection's fd is readable: it registers the fd with Linux::IRunLoop on the
/// IPlugFrame, the same run loop as with kPlatformTypeX11EmbedWindowID.
///
/// The view's size is in logical pixels, unlike kPlatformTypeX11EmbedWindowID's: the plugin gets
/// the scale from the compositor (wl_surface.preferred_buffer_scale or
/// wp_fractional_scale_v1.preferred_scale), as any Wayland client. The host doesn't call
/// IPlugViewContentScaleSupport::setContentScaleFactor() for this platform type.
///
/// The keyboard focus is the compositor's, which is the host: when the plugin's surface has it,
/// the plugin gets the keys from its own wl_keyboard, and the host doesn't call
/// IPlugView::onKeyDown() and onKeyUp().

namespace WaylandExperiment {

/** The parent parameter of IPlugView::attached() is a const char *: the embedding token for
 * clap_surface_embedding_v1.get_embedded_surface(), null-terminated, and only valid during the
 * call. The plugin connects to the host's compositor with
 * IWaylandNestedCompositor::openConnection(), never to the system compositor.
 *
 * The token tells the host's compositor where to embed the surface, so one connection can serve
 * all the plugin's views in this host.
 *
 * The plugin returns kResultTrue from IPlugView::isPlatformTypeSupported() only if the host
 * implements IWaylandNestedCompositor. */
const Steinberg::FIDString kPlatformTypeWaylandSurfaceEmbeddingV1 = "WaylandSurfaceEmbeddingV1";

/** IWaylandNestedCompositor: the host's Wayland compositor, for kPlatformTypeWaylandSurfaceEmbeddingV1.

Implemented by the host's IHostApplication, the context given to IPluginBase::initialize() and
IPluginFactory3::setHostContext(): the plugin gets it with queryInterface().

- [host imp]
- [extends IHostApplication]
*/
class IWaylandNestedCompositor : public Steinberg::FUnknown
{
public:
	/** Opens a new connection to the host's Wayland compositor.
	 *
	 * Returns the file descriptor of a connected socket, or -1 on failure.
	 *
	 * The plugin becomes the owner of the file descriptor, and is responsible for closing it. It
	 * may connect with wl_display_connect_to_fd(), then wl_display_disconnect() closes it. It may
	 * also pass it to another process, for example in the WAYLAND_SOCKET environment variable,
	 * and close its own copy.
	 *
	 * All the connections lead to the same compositor, so a plugin may use one connection for all
	 * its views in this host.
	 * \note [UI-thread] */
	virtual Steinberg::int32 PLUGIN_API openConnection () = 0;

	static const Steinberg::FUID iid;
};

DECLARE_CLASS_IID (IWaylandNestedCompositor, 0xE25143B4, 0x9BC445E4, 0x935DF7EB, 0x88875AC0)

} // namespace WaylandExperiment

#include "pluginterfaces/base/falignpop.h"
