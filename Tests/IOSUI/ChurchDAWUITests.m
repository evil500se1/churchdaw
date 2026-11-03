#import <XCTest/XCTest.h>

@interface ChurchDAWUITests : XCTestCase
@end

@implementation ChurchDAWUITests

- (void)setUp
{
    [super setUp];
    self.continueAfterFailure = NO;
}

- (void)keepScreenshotNamed:(NSString *)name
{
    XCUIScreenshot *screenshot = XCUIScreen.mainScreen.screenshot;
    XCTAttachment *attachment = [XCTAttachment attachmentWithScreenshot:screenshot];
    attachment.name = name;
    attachment.lifetime = XCTAttachmentLifetimeKeepAlways;
    [self addAttachment:attachment];
}

- (void)keepAccessibilityTreeForApplication:(XCUIApplication *)app
                                      named:(NSString *)name
{
    XCTAttachment *tree = [XCTAttachment attachmentWithString:app.debugDescription];
    tree.name = name;
    tree.lifetime = XCTAttachmentLifetimeKeepAlways;
    [self addAttachment:tree];
}

- (XCUIApplication *)launchChurchDAWInLandscape
{
    XCUIDevice.sharedDevice.orientation = UIDeviceOrientationLandscapeLeft;

    XCUIApplication *app = [[XCUIApplication alloc]
        initWithBundleIdentifier:@"com.churchdaw.app"];
    [app launch];
    XCTAssertTrue([app waitForExistenceWithTimeout:15.0],
                  @"O ChurchDAW nao abriu no aparelho de QA");

    XCUIElement *openWithoutDownload = app.buttons[@"Abrir sem baixar"];
    if ([openWithoutDownload waitForExistenceWithTimeout:2.0])
        [openWithoutDownload tap];

    return app;
}

- (XCUIElement *)openContinuousPadInApplication:(XCUIApplication *)app
{
    XCUIElement *pad = app.buttons[@"Pad"];
    XCTAssertTrue([pad waitForExistenceWithTimeout:15.0],
                  @"O botao Pad sumiu da interface");
    [pad tap];

    XCUIElement *continuous = app.buttons[@"PAD CONTÍNUO"];
    XCTAssertTrue([continuous waitForExistenceWithTimeout:10.0],
                  @"A aba Pad Continuo sumiu do painel");
    [continuous tap];

    [NSThread sleepForTimeInterval:1.0];
    XCTAssertTrue(app.exists,
                  @"O ChurchDAW fechou ao abrir o Pad Continuo");
    XCTAssertEqual(app.alerts.count, 0,
                   @"O Pad Continuo nao encontrou os sete WAVs de QA");

    // O PadPanel e um componente JUCE desenhado a mao. Ele aparece na arvore
    // como o Other de 640x470 pontos; os doze tons ainda nao sao elementos
    // UIKit separados. Resolver o card pela geometria evita um falso positivo
    // em que o teste apenas abre o painel sem nunca tocar no motor de audio.
    XCUIElement *panel = nil;
    CGFloat bestScore = CGFLOAT_MAX;

    for (XCUIElement *candidate in app.otherElements.allElementsBoundByIndex)
    {
        const CGRect frame = candidate.frame;
        const CGFloat score = fabs(frame.size.width - 640.0)
                            + fabs(frame.size.height - 470.0);

        if (score < bestScore)
        {
            panel = candidate;
            bestScore = score;
        }
    }

    XCTAssertNotNil(panel, @"O card do Pad Continuo nao apareceu");
    XCTAssertLessThan(bestScore, 6.0,
                      @"O card do Pad Continuo apareceu com geometria inesperada");
    return panel;
}

- (void)tapContinuousPadTone:(NSInteger)tone onPanel:(XCUIElement *)panel
{
    // Centros da grade 3x4 calculada por PadPanel::resized(), normalizados no
    // proprio card de 640x470. A ordem e C, C#, D, D#, E, F, F#, G, G#, A,
    // A#, B, igual ao indice aceito por PadProcessor::setKey().
    static const CGFloat columns[3] = { 0.1943, 0.5000, 0.8057 };
    static const CGFloat rows[4] = { 0.3184, 0.4593, 0.6003, 0.7412 };
    const NSInteger safeTone = MAX (0, MIN (11, tone));
    const CGVector offset = CGVectorMake(columns[safeTone % 3],
                                         rows[safeTone / 3]);
    [[panel coordinateWithNormalizedOffset:offset] tap];
}

- (void)testLaunchAndCaptureInterface
{
    XCUIDevice.sharedDevice.orientation = UIDeviceOrientationLandscapeLeft;

    XCUIApplication *app = [[XCUIApplication alloc]
        initWithBundleIdentifier:@"com.churchdaw.app"];
    [app launch];

    XCTAssertTrue([app waitForExistenceWithTimeout:15.0],
                  @"O ChurchDAW nao abriu no aparelho fisico");
    [NSThread sleepForTimeInterval:4.0];

    [self keepAccessibilityTreeForApplication:app
                                        named:@"ChurchDAW-accessibility-tree"];
    [self keepScreenshotNamed:@"ChurchDAW-launch-landscape"];
}

- (void)testOpenWithoutSamplesAndCaptureMainInterface
{
    XCUIDevice.sharedDevice.orientation = UIDeviceOrientationLandscapeLeft;

    XCUIApplication *app = [[XCUIApplication alloc]
        initWithBundleIdentifier:@"com.churchdaw.app"];
    [app launch];
    XCTAssertTrue([app waitForExistenceWithTimeout:15.0],
                  @"O ChurchDAW nao abriu no aparelho de QA");

    XCUIElement *openWithoutDownload = app.buttons[@"Abrir sem baixar"];
    if ([openWithoutDownload waitForExistenceWithTimeout:10.0])
        [openWithoutDownload tap];

    [NSThread sleepForTimeInterval:8.0];
    XCTAssertTrue(app.exists, @"O ChurchDAW fechou ao entrar sem samples");

    [self keepAccessibilityTreeForApplication:app
                                        named:@"ChurchDAW-main-interface-tree"];
    [self keepScreenshotNamed:@"ChurchDAW-main-interface-landscape"];
}

- (void)testStageModeIsPresentAndOpens
{
    XCUIApplication *app = [self launchChurchDAWInLandscape];

    XCUIElement *stageMode = app.buttons[@"Modo palco"];
    if (! [stageMode waitForExistenceWithTimeout:3.0])
    {
        XCUIElement *instruments = app.buttons[@"Instrumentos"];
        XCTAssertTrue([instruments waitForExistenceWithTimeout:10.0],
                      @"O seletor Instrumentos sumiu da interface");
        [instruments tap];

        XCUIElement *openInterface = app.buttons[@"Abrir interface"];
        XCTAssertTrue([openInterface waitForExistenceWithTimeout:10.0],
                      @"A interface do Church Live nao apareceu");
        [openInterface tap];
    }

    XCTAssertTrue([stageMode waitForExistenceWithTimeout:15.0],
                  @"O botao Modo palco sumiu da interface do iPad");
    XCTAssertTrue(stageMode.enabled,
                  @"O botao Modo palco ficou desabilitado sem timbre");

    // A assercao acima protege o primeiro uso, ainda sem timbre. Em seguida o
    // simulador recebe um WAV sintetico de QA em "QA Stage Piano" para tambem
    // validar o fluxo real com um canal carregado e meters ativos.
    XCUIElement *addInstrument = app.buttons[@"Adicionar"].firstMatch;
    if ([addInstrument waitForExistenceWithTimeout:15.0])
    {
        [addInstrument tap];

        // A busca JUCE e exposta pelo iOS como TextView, sem entrada de texto
        // automatizavel. Filtrar por Pianos remove os cabecalhos e deixa o
        // timbre local "QA Stage Piano" como primeira linha deterministica.
        XCUIElement *pianos = app.buttons[@"Pianos"];
        XCTAssertTrue([pianos waitForExistenceWithTimeout:10.0],
                      @"O filtro Pianos sumiu do seletor");
        [pianos tap];

        XCUIElement *qaTimbreRow = app.otherElements[@"Row 1"];
        XCTAssertTrue([qaTimbreRow waitForExistenceWithTimeout:10.0],
                      @"O timbre QA nao apareceu no seletor");
        // O seletor usa clique simples para destacar e duplo para confirmar.
        [qaTimbreRow doubleTap];

        XCTAssertFalse([qaTimbreRow waitForExistenceWithTimeout:10.0],
                       @"O timbre QA nao foi confirmado no canal");
        [NSThread sleepForTimeInterval:3.0];
    }

    // Um instrumento Church Organ pode reabrir seu painel embutido ao
    // restaurar uma sessao anterior. Voltar ao mixer antes do toque impede que
    // o botao sobreposto "MIXER" receba o gesto destinado ao Modo Palco.
    XCUIElement *returnToMixer = app.buttons[@"← MIXER"];
    if ([returnToMixer waitForExistenceWithTimeout:2.0])
        [returnToMixer tap];

    [stageMode tap];

    XCUIElement *editChannel = app.buttons[@"Editar canal"];
    XCTAssertTrue([editChannel waitForExistenceWithTimeout:10.0],
                  @"O toque nao entrou de verdade no Modo palco");
    XCTAssertTrue(app.exists, @"O ChurchDAW fechou ao abrir o Modo palco");
    XCUIElement *masterTitle = [[app.staticTexts
        matchingPredicate:[NSPredicate predicateWithFormat:@"value == %@", @"MASTER"]]
        firstMatch];
    XCTAssertTrue([masterTitle waitForExistenceWithTimeout:5.0],
                  @"A regua Master sumiu do Modo palco");
    XCTAssertTrue([app.buttons[@"PANIC"] waitForExistenceWithTimeout:5.0],
                  @"O PANIC sumiu do Modo palco");

    [self keepAccessibilityTreeForApplication:app
                                        named:@"ChurchDAW-stage-mode-tree"];
    [self keepScreenshotNamed:@"ChurchDAW-stage-mode-landscape"];
}

- (void)testCaptureInstrumentPicker
{
    XCUIDevice.sharedDevice.orientation = UIDeviceOrientationLandscapeLeft;
    XCUIApplication *app = [[XCUIApplication alloc]
        initWithBundleIdentifier:@"com.churchdaw.app"];
    [app launch];
    XCTAssertTrue([app waitForExistenceWithTimeout:15.0],
                  @"O ChurchDAW nao abriu no aparelho de QA");

    XCUIElement *instruments = app.buttons[@"Instrumentos"];
    XCTAssertTrue([instruments waitForExistenceWithTimeout:15.0],
                  @"O seletor Instrumentos sumiu da interface");
    [instruments tap];
    [NSThread sleepForTimeInterval:4.0];

    [self keepAccessibilityTreeForApplication:app
                                        named:@"ChurchDAW-instrument-picker-tree"];
    [self keepScreenshotNamed:@"ChurchDAW-instrument-picker-landscape"];
}

- (void)testCapturePadPanel
{
    XCUIDevice.sharedDevice.orientation = UIDeviceOrientationLandscapeLeft;
    XCUIApplication *app = [[XCUIApplication alloc]
        initWithBundleIdentifier:@"com.churchdaw.app"];
    [app launch];
    XCTAssertTrue([app waitForExistenceWithTimeout:15.0],
                  @"O ChurchDAW nao abriu no aparelho de QA");

    XCUIElement *pad = app.buttons[@"Pad"];
    XCTAssertTrue([pad waitForExistenceWithTimeout:15.0],
                  @"O botao Pad sumiu da interface");
    [pad tap];
    [NSThread sleepForTimeInterval:4.0];
    XCTAssertTrue(app.exists, @"O ChurchDAW fechou ao abrir o painel Pad");

    [self keepAccessibilityTreeForApplication:app
                                        named:@"ChurchDAW-pad-panel-tree"];
    [self keepScreenshotNamed:@"ChurchDAW-pad-panel-landscape"];
}

- (void)testCaptureContinuousPadPanel
{
    XCUIApplication *app = [self launchChurchDAWInLandscape];
    [self openContinuousPadInApplication:app];
    [NSThread sleepForTimeInterval:3.0];

    [self keepAccessibilityTreeForApplication:app
                                        named:@"ChurchDAW-continuous-pad-tree"];
    [self keepScreenshotNamed:@"ChurchDAW-continuous-pad-landscape"];
}

- (void)testContinuousPadRapidToneChangesDoNotCrash
{
    XCUIApplication *app = [self launchChurchDAWInLandscape];
    XCUIElement *panel = [self openContinuousPadInApplication:app];

    // Primeiro carregamento completo: prova o caminho WAV -> loader -> voz.
    [self tapContinuousPadTone:0 onPanel:panel];
    [NSThread sleepForTimeInterval:2.0];
    XCTAssertEqual(app.state, XCUIApplicationStateRunningForeground,
                   @"O ChurchDAW fechou ao ligar o Pad Continuo em C");
    [self keepScreenshotNamed:@"ChurchDAW-continuous-pad-C-playing"];

    // O relato original acontecia ao ligar/trocar notas. Cinco voltas em ida
    // e volta forcam cancelamento de loads antigos, crossfades e reutilizacao
    // das vozes sem dar tempo artificial entre cada toque.
    for (NSInteger round = 0; round < 5; ++round)
    {
        for (NSInteger tone = 0; tone < 12; ++tone)
        {
            [self tapContinuousPadTone:tone onPanel:panel];
            [NSThread sleepForTimeInterval:0.08];
        }

        for (NSInteger tone = 10; tone >= 0; --tone)
        {
            [self tapContinuousPadTone:tone onPanel:panel];
            [NSThread sleepForTimeInterval:0.08];
        }

        XCTAssertEqual(app.state, XCUIApplicationStateRunningForeground,
                       @"O ChurchDAW fechou durante trocas rapidas do Pad Continuo");
    }

    [NSThread sleepForTimeInterval:3.0];
    XCTAssertEqual(app.state, XCUIApplicationStateRunningForeground,
                   @"O ChurchDAW fechou depois de estabilizar o ultimo tom");

    XCUIElement *stop = app.buttons[@"Parar"];
    XCTAssertTrue([stop waitForExistenceWithTimeout:3.0],
                  @"O controle Parar sumiu do Pad Continuo");
    [stop tap];
    [NSThread sleepForTimeInterval:1.0];
    XCTAssertEqual(app.state, XCUIApplicationStateRunningForeground,
                   @"O ChurchDAW fechou ao parar o Pad Continuo");

    [self keepAccessibilityTreeForApplication:app
                                        named:@"ChurchDAW-continuous-pad-stress-tree"];
    [self keepScreenshotNamed:@"ChurchDAW-continuous-pad-after-stress"];
}

- (void)testDiscoversInstalledThirdPartyAUv3
{
    XCUIApplication *app = [self launchChurchDAWInLandscape];

    NSPredicate *pluginsPredicate =
        [NSPredicate predicateWithFormat:@"label ==[c] %@", @"Plugins"];
    XCUIElement *plugins =
        [[app.buttons matchingPredicate:pluginsPredicate] firstMatch];
    XCTAssertTrue([plugins waitForExistenceWithTimeout:15.0],
                  @"O botao Plugins sumiu da interface do iPad");
    [plugins tap];

    NSPredicate *bs16Predicate =
        [NSPredicate predicateWithFormat:@"label CONTAINS[c] %@", @"bs-16i"];
    XCUIElement *bs16 =
        [[[app descendantsMatchingType:XCUIElementTypeAny]
            matchingPredicate:bs16Predicate] firstMatch];

    if (! [bs16 waitForExistenceWithTimeout:5.0])
    {
        NSPredicate *scanPredicate =
            [NSPredicate predicateWithFormat:@"label BEGINSWITH[c] %@", @"ESCANEAR"];
        XCUIElement *scan =
            [[app.buttons matchingPredicate:scanPredicate] firstMatch];

        XCTAssertTrue([scan waitForExistenceWithTimeout:30.0],
                      @"O gerenciador AUv3 nao terminou nem ofereceu novo scan");

        if (! bs16.exists && scan.enabled)
            [scan tap];
    }

    XCTAssertTrue([bs16 waitForExistenceWithTimeout:90.0],
                  @"O AUv3 externo bs-16i instalado nao apareceu no ChurchDAW");
    XCTAssertTrue(app.buttons[@"ABRIR PLUGIN"].exists,
                  @"O gerenciador AUv3 nao exibiu a acao de abertura");

    [self keepAccessibilityTreeForApplication:app
                                        named:@"ChurchDAW-third-party-AUv3-tree"];
    [self keepScreenshotNamed:@"ChurchDAW-third-party-AUv3-discovered"];
}

@end
