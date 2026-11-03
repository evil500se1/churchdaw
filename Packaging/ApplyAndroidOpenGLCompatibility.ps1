param([string] $ProjectRoot = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'

# JUCE 9 queries texture dimensions with glGetTexLevelParameteriv, introduced
# in OpenGL ES 3.1. Android's JUCE context may be ES 3.0: the query leaves both
# dimensions zero and silently drops uploaded camera, UI and gradient images.
# Keep the dimensions of the successful glTexImage2D allocation instead.
$module = Join-Path $ProjectRoot 'JUCE\modules\juce_opengl\opengl'
$framebuffer = Join-Path $module 'juce_OpenGLFrameBuffer.cpp'
$texture = Join-Path $module 'juce_OpenGLTexture.cpp'
$utf8 = [Text.UTF8Encoding]::new($false)
$f = [IO.File]::ReadAllText($framebuffer).Replace("`r`n", "`n")
$t = [IO.File]::ReadAllText($texture).Replace("`r`n", "`n")
if ($f.Contains('allocatedWidth') -and $t.Contains('ChurchDAW ES 3.0 texture allocation')) { return }
if ($f.Contains('allocatedWidth') -or $t.Contains('ChurchDAW ES 3.0 texture allocation')) { throw 'Incomplete JUCE compatibility patch' }
$beforeF = 'static bool tryAllocTexture (int w, int h, GLenum type)'
$beforeReturn = "        if (error == GL_NO_ERROR)`n            return true;"
$beforeQuery = @'
    GLint detectedWidth{}, detectedHeight{};
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &detectedWidth);
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &detectedHeight);
    std::tie (width, height) = std::tie (detectedWidth, detectedHeight);
'@
$beforeQuery = $beforeQuery.Replace("`r`n", "`n")
foreach ($entry in @(@($f,$beforeF),@($f,$beforeReturn),@($t,'tryAllocTexture (w, h, type)'),@($t,$beforeQuery))) {
    if (-not $entry[0].Contains($entry[1])) { throw 'JUCE texture code changed; review compatibility patch before building' }
}
$f=$f.Replace($beforeF,'static bool tryAllocTexture (int w, int h, GLenum type, int* allocatedWidth = nullptr, int* allocatedHeight = nullptr)')
$f=$f.Replace($beforeReturn,@'
        if (error == GL_NO_ERROR)
        {
            if (allocatedWidth != nullptr) *allocatedWidth = testWidth;
            if (allocatedHeight != nullptr) *allocatedHeight = testHeight;
            return true;
        }
'@)
$t=$t.Replace('tryAllocTexture (w, h, type)','tryAllocTexture (w, h, type, &width, &height)')
$t=$t.Replace($beforeQuery,'    // ChurchDAW ES 3.0 texture allocation: dimensions returned by allocator.')
[IO.File]::WriteAllText($framebuffer,$f.Replace("`r`n","`n").Replace("`n","`r`n"),$utf8)
[IO.File]::WriteAllText($texture,$t.Replace("`n","`r`n"),$utf8)
Write-Host 'Android OpenGL ES 3.0 image upload compatibility applied.'
