#pragma once

// Isolated PC mobile-shell preview toggle. Always 0 in every production binary
// and desktop test; enabled only in the standalone ChurchDAWMobileDesignPreview
// target so mobile layout/touch surfaces can be inspected and captured on PC.
#ifndef CHURCHDAW_MOBILE_UI_PREVIEW
 #define CHURCHDAW_MOBILE_UI_PREVIEW 0
#endif

#if JUCE_ANDROID || JUCE_IOS || CHURCHDAW_MOBILE_UI_PREVIEW
 #define CHURCHDAW_MOBILE_SHELL 1
#else
 #define CHURCHDAW_MOBILE_SHELL 0
#endif
