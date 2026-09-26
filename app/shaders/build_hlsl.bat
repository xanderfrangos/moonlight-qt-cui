fxc /T vs_5_0 /O3 /Fo d3d11_vertex.fxc d3d11_vertex.hlsl

fxc /T ps_5_0 /O3 /Fo d3d11_overlay_pixel.fxc d3d11_overlay_pixel.hlsl
fxc /T ps_5_0 /O3 /Fo d3d11_yuv420_pixel.fxc d3d11_yuv420_pixel.hlsl
fxc /T ps_5_0 /O3 /Fo d3d11_ayuv_pixel.fxc d3d11_ayuv_pixel.hlsl
fxc /T ps_5_0 /O3 /Fo d3d11_y410_pixel.fxc d3d11_y410_pixel.hlsl
fxc /T ps_5_0 /O3 /Fo d3d11_yuv_planar_pixel.fxc d3d11_yuv_planar_pixel.hlsl

rem Dithering variants of the video shaders, built from the same sources
fxc /T ps_5_0 /O3 /D DITHER_OUTPUT=1 /Fo d3d11_yuv420_dither_pixel.fxc d3d11_yuv420_pixel.hlsl
fxc /T ps_5_0 /O3 /D DITHER_OUTPUT=1 /Fo d3d11_ayuv_dither_pixel.fxc d3d11_ayuv_pixel.hlsl
fxc /T ps_5_0 /O3 /D DITHER_OUTPUT=1 /Fo d3d11_y410_dither_pixel.fxc d3d11_y410_pixel.hlsl

rem FidelityFX Super Resolution 1.0 passes, built from one source
fxc /T ps_5_0 /O3 /D APPLY_EASU=1 /Fo d3d11_fsr1_easu_pixel.fxc d3d11_fsr1_pixel.hlsl
fxc /T ps_5_0 /O3 /D APPLY_EASU=1 /D FSR_PQ=1 /Fo d3d11_fsr1_easu_pq_pixel.fxc d3d11_fsr1_pixel.hlsl
fxc /T ps_5_0 /O3 /D APPLY_RCAS=1 /Fo d3d11_fsr1_rcas_pixel.fxc d3d11_fsr1_pixel.hlsl
fxc /T ps_5_0 /O3 /D APPLY_RCAS=1 /D FSR_PQ=1 /Fo d3d11_fsr1_rcas_pq_pixel.fxc d3d11_fsr1_pixel.hlsl
fxc /T ps_5_0 /O3 /D APPLY_RCAS=1 /D DITHER_OUTPUT=1 /Fo d3d11_fsr1_rcas_dither_pixel.fxc d3d11_fsr1_pixel.hlsl

rem Copy from an upscaler output texture to the back buffer
fxc /T ps_5_0 /O3 /Fo d3d11_upscale_copy_pixel.fxc d3d11_upscale_copy_pixel.hlsl
fxc /T ps_5_0 /O3 /D DITHER_OUTPUT=1 /Fo d3d11_upscale_copy_dither_pixel.fxc d3d11_upscale_copy_pixel.hlsl
