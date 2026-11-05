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

// Identidade visual Church Live (paleta, raios e tipografia Barlow). Vale no
// shell mobile e, por padrao, tambem no desktop; o LAYOUT do desktop segue
// independente (CHURCHDAW_MOBILE_SHELL). Defina CHURCHDAW_DESKTOP_CHURCHLIVE_LOOK=0
// para voltar a paleta/fonte anteriores do desktop.
#ifndef CHURCHDAW_DESKTOP_CHURCHLIVE_LOOK
 #define CHURCHDAW_DESKTOP_CHURCHLIVE_LOOK 1
#endif

#if CHURCHDAW_MOBILE_SHELL || CHURCHDAW_DESKTOP_CHURCHLIVE_LOOK
 #define CHURCHDAW_CHURCHLIVE_LOOK 1
#else
 #define CHURCHDAW_CHURCHLIVE_LOOK 0
#endif
