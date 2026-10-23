// Derived from the pinned GSRenderer::PresentCurrentFrame and VSync presentation
// path. SPDX-License-Identifier: GPL-3.0-or-later
void GSRenderer::PresentCurrentFrame()
{
    tc3::preview::Prepare();
    GSTexture* current = g_gs_device->GetCurrent();
    GSVector4i src_rect;
    GSVector4 src_uv, draw_rect;
    if (current)
    {
        src_rect = CalculateDrawSrcRect(current, m_real_size);
        src_uv = GSVector4(src_rect) / GSVector4(current->GetSize()).xyxy();
        draw_rect = CalculateDrawDstRect(g_gs_device->GetWindowWidth(), g_gs_device->GetWindowHeight(),
            src_rect, current->GetSize(), s_display_alignment, g_gs_device->UsesLowerLeftOrigin(),
            GetVideoMode() == GSVideoMode::SDTV_480P);
        s_last_draw_rect = draw_rect;
        // Match normal VSync: compute must be outside the present render pass.
        if (GSConfig.CASMode != GSCASMode::Disabled && g_gs_device->Features().cas_sharpening)
        {
            const bool sharpen_only = GSConfig.CASMode == GSCASMode::SharpenOnly ||
                (current->GetWidth() > g_gs_device->GetWindowWidth() &&
                 current->GetHeight() > g_gs_device->GetWindowHeight());
            g_gs_device->CAS(current, src_rect, src_uv, draw_rect, sharpen_only);
        }
    }
    if (BeginPresentFrame(false))
    {
        if (current)
        {
            const u64 current_time = Common::Timer::GetCurrentValue();
            const float shader_time = static_cast<float>(Common::Timer::ConvertValueToSeconds(current_time - m_shader_time_start));
            g_gs_device->PresentRect(current, src_uv, nullptr, draw_rect,
                s_tv_shader_indices[GSConfig.TVShader], shader_time, BilnIf(GSConfig.LinearPresent != GSPostBilinearMode::Off));
        }
        EndPresentFrame();
    }
}
