<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

final class App
{
    private Config $config;
    private Database $database;
    private AuthService $auth;
    private PaymentService $payments;
    private WebSessionService $sessions;
    private LicenseService $licenses;
    private TrialService $trials;
    private LibraryDownloadService $libraryDownloads;
    private AdminService $admin;
    private AdminViews $adminViews;
    private Views $views;

    public function __construct(private readonly string $projectRoot)
    {
        $this->config = Config::load($projectRoot);
        $this->config->assertRuntimeReady();
        $database = new Database($this->config);
        $this->database = $database;
        $crypto = new Crypto($this->config);
        $limiter = new RateLimiter($database, $crypto);
        $mailer = new Mailer($this->config, $database, $crypto);
        $audit = new AuditLog($database, $crypto);
        $this->licenses = new LicenseService($this->config, $database, $crypto);
        $this->trials = new TrialService(
            $this->config,
            $database,
            $crypto,
            $limiter
        );
        $this->libraryDownloads = new LibraryDownloadService(
            $this->config,
            $crypto,
            $limiter
        );
        $this->auth = new AuthService(
            $database,
            $limiter,
            $this->licenses,
            $mailer,
            $audit
        );
        $this->payments = new PaymentService(
            $this->config,
            $database,
            $crypto,
            $limiter,
            new MercadoPagoClient($this->config),
            $mailer
        );
        $this->sessions = new WebSessionService(
            $this->config,
            $database,
            $crypto,
            $limiter
        );
        $this->admin = new AdminService(
            $this->config,
            $database,
            $crypto,
            $limiter
        );
        $this->adminViews = new AdminViews($this->config);
        $this->views = new Views($this->config);
    }

    public function run(): never
    {
        $requestId = Crypto::randomToken(16);
        $requestPath = '/' . ltrim((string) parse_url(
            (string) ($_SERVER['REQUEST_URI'] ?? '/'),
            PHP_URL_PATH
        ), '/');
        try {
            $request = Request::fromGlobals($this->config);
            $requestId = $request->requestId;
            $requestPath = $request->path;
            $this->assertHttps();
            $route = $request->method . ' ' . rtrim($request->path, '/');
            if ($route === 'GET') {
                $route = 'GET /';
            }

            if ($request->method === 'GET'
                && preg_match(
                    '#^/api/v1/library/files/([A-Za-z0-9][A-Za-z0-9._-]{1,150}\.zip)$#',
                    $request->path,
                    $downloadMatch
                )) {
                $this->libraryDownloads->stream($request, $downloadMatch[1]);
            }
            if ($request->path === '/admin'
                || str_starts_with($request->path, '/admin/')) {
                $this->runAdmin($request);
            }

            match ($route) {
                'GET /', 'GET /comprar' => $this->purchasePage($request),
                'GET /entrar' => Response::html(
                    $this->views->login(),
                    200,
                    $request->requestId
                ),
                'GET /esqueci-senha' => Response::html(
                    $this->views->forgotPassword(),
                    200,
                    $request->requestId
                ),
                'GET /reenviar-confirmacao' => Response::html(
                    $this->views->resendVerification(),
                    200,
                    $request->requestId
                ),
                'GET /termos' => Response::html(
                    $this->views->terms(),
                    200,
                    $request->requestId
                ),
                'GET /politica-de-privacidade' => Response::html(
                    $this->views->privacy(),
                    200,
                    $request->requestId
                ),
                'GET /minha-conta' => Response::html(
                    $this->views->account(),
                    200,
                    $request->requestId
                ),
                'GET /redefinir-senha' => Response::html(
                    $this->views->resetPassword(),
                    200,
                    $request->requestId
                ),
                'GET /verificar-email' => Response::html(
                    $this->views->verifyEmail(),
                    200,
                    $request->requestId
                ),
                'GET /compra/sucesso' => Response::html(
                    $this->views->message(
                        'Pagamento recebido',
                        'Assim que o pagamento for confirmado, sua licença aparecerá na conta. Confirme também seu e-mail.'
                    ),
                    200,
                    $request->requestId
                ),
                'GET /compra/pendente' => Response::html(
                    $this->views->message(
                        'Pagamento pendente',
                        'Pix ou boleto pode levar alguns instantes para confirmar. Não faça um novo pagamento.',
                        'pending'
                    ),
                    200,
                    $request->requestId
                ),
                'GET /compra/falhou' => Response::html(
                    $this->views->message(
                        'Pagamento não concluído',
                        'Nenhuma licença foi cobrada. Você pode tentar novamente.',
                        'error'
                    ),
                    200,
                    $request->requestId
                ),
                'GET /api/v1/health' => $this->health($request),
                'POST /api/v1/auth/login' => Response::json(
                    $this->auth->login($request),
                    200,
                    $request->requestId
                ),
                'POST /api/v1/auth/refresh' => Response::json(
                    $this->auth->refresh($request),
                    200,
                    $request->requestId
                ),
                'POST /api/v1/trial/reserve' => Response::json(
                    $this->trials->reserve($request),
                    200,
                    $request->requestId
                ),
                'POST /api/v1/trial/start' => Response::json(
                    $this->trials->start($request),
                    200,
                    $request->requestId
                ),
                'POST /api/v1/auth/password/forgot' => $this->forgotPassword($request),
                'POST /api/v1/auth/password/reset' => $this->resetPassword($request),
                'POST /api/v1/auth/email/resend' => $this->resendVerification($request),
                'POST /api/v1/auth/email/verify' => $this->verifyEmail($request),
                'POST /api/v1/checkout/start' =>
                    $this->startCheckout($request),
                'POST /api/v1/webhooks/mercado-pago' => $this->mercadoPagoWebhook($request),
                'POST /api/v1/web/login' => Response::json(
                    $this->sessions->login($request),
                    200,
                    $request->requestId
                ),
                'POST /api/v1/web/logout' => $this->logout($request),
                'GET /api/v1/account' => Response::json(
                    $this->sessions->account($request),
                    200,
                    $request->requestId
                ),
                'POST /api/v1/account/device/deactivate' => $this->deactivateDevice($request),
                default => throw new ApiException(404, 'NOT_FOUND', 'Rota inexistente.'),
            };
        } catch (ApiException $error) {
            if (str_starts_with($requestPath, '/api/')) {
                Response::json([
                    'error' => $error->apiCode,
                    'code' => $error->apiCode,
                    'message' => $error->getMessage(),
                    'request_id' => $requestId,
                ], $error->status, $requestId);
            }
            Response::html(
                $this->views->message('Não foi possível continuar', $error->getMessage(), 'error'),
                $error->status,
                $requestId
            );
        } catch (\Throwable $error) {
            error_log(sprintf(
                'ChurchDAW request=%s error=%s at %s:%d',
                $requestId,
                $error->getMessage(),
                $error->getFile(),
                $error->getLine()
            ));
            if (str_starts_with($requestPath, '/api/')) {
                Response::json([
                    'error' => 'INTERNAL_ERROR',
                    'code' => 'INTERNAL_ERROR',
                    'message' => 'Não foi possível concluir agora.',
                    'request_id' => $requestId,
                ], 500, $requestId);
            }
            Response::html(
                $this->views->message(
                    'Serviço temporariamente indisponível',
                    'Tente novamente em alguns minutos.',
                    'error'
                ),
                500,
                $requestId
            );
        }
    }

    private function purchasePage(Request $request): never
    {
        $enabled = $this->config->bool('checkout.enabled');
        Response::html(
            $enabled
                ? $this->views->purchase()
                : $this->views->message(
                    'Compras temporariamente pausadas',
                    'Estamos preparando a proxima liberacao do ChurchDAW. '
                        . 'Se voce ja comprou, sua conta continua disponivel.',
                    'pending'
                ),
            $enabled ? 200 : 503,
            $request->requestId
        );
    }

    private function runAdmin(Request $request): never
    {
        $route = $request->method . ' ' . rtrim($request->path, '/');
        if ($route === 'GET') {
            $route = 'GET /admin';
        }
        if ($route === 'GET /admin') {
            try {
                Response::html(
                    $this->adminViews->dashboard(
                        $this->admin->dashboard($request)
                    ),
                    200,
                    $request->requestId
                );
            } catch (ApiException $error) {
                if ($error->status !== 401) {
                    throw $error;
                }
                Response::html(
                    $this->adminViews->login(),
                    200,
                    $request->requestId
                );
            }
        }
        if ($route === 'POST /admin/login') {
            try {
                $this->admin->login($request);
                Response::redirect('/admin', 303, $request->requestId);
            } catch (ApiException $error) {
                if ($error->status !== 401 && $error->status !== 429) {
                    throw $error;
                }
                Response::html(
                    $this->adminViews->login($error->getMessage()),
                    $error->status,
                    $request->requestId
                );
            }
        }
        if ($route === 'POST /admin/logout') {
            $this->admin->logout($request);
            Response::redirect('/admin', 303, $request->requestId);
        }
        if ($route === 'POST /admin/licenses/issue') {
            $this->admin->issueLicense($request);
            Response::redirect('/admin', 303, $request->requestId);
        }
        if ($request->method === 'POST'
            && preg_match(
                '#^/admin/licenses/([0-9a-f-]{36})/revoke$#i',
                $request->path,
                $match
            )) {
            $this->admin->revokeLicense($request, $match[1]);
            Response::redirect('/admin', 303, $request->requestId);
        }
        if ($request->method === 'POST'
            && preg_match(
                '#^/admin/devices/([0-9a-f-]{36})/deactivate$#i',
                $request->path,
                $match
            )) {
            $this->admin->deactivateDevice($request, $match[1]);
            Response::redirect('/admin', 303, $request->requestId);
        }
        if ($request->method === 'POST'
            && preg_match(
                '#^/admin/users/([0-9a-f-]{36})/reset-access$#i',
                $request->path,
                $match
            )) {
            $this->admin->resetCustomerAccess($request, $match[1]);
            Response::redirect('/admin', 303, $request->requestId);
        }
        throw new ApiException(404, 'NOT_FOUND', 'Rota inexistente.');
    }

    private function startCheckout(Request $request): never
    {
        if (!$this->config->bool('checkout.enabled')) {
            throw new ApiException(
                503,
                'CHECKOUT_DISABLED',
                'As compras estao temporariamente pausadas.'
            );
        }
        Response::json(
            $this->payments->startCheckout($request),
            201,
            $request->requestId
        );
    }

    private function health(Request $request): never
    {
        $schema = MigrationState::inspect(
            $this->database->pdo(),
            $this->projectRoot
        );

        Response::json([
            'ok' => $schema['ready'],
            'service' => 'churchdaw-licensing',
            'release' => $this->config->string('app.release'),
            'schema' => $schema,
            'checkout_enabled' => $this->config->bool('checkout.enabled'),
            'time' => gmdate(DATE_ATOM),
        ], $schema['ready'] ? 200 : 503, $request->requestId);
    }

    private function forgotPassword(Request $request): never
    {
        $this->auth->requestPasswordReset($request);
        Response::json([
            'ok' => true,
            'message' => 'Se o e-mail estiver cadastrado, enviaremos as instruções.',
        ], 202, $request->requestId);
    }

    private function resetPassword(Request $request): never
    {
        $this->auth->resetPassword($request);
        Response::json(['ok' => true], 200, $request->requestId);
    }

    private function resendVerification(Request $request): never
    {
        $this->auth->requestEmailVerification($request);
        Response::json([
            'ok' => true,
            'message' => 'Se a conta ainda precisar de confirmacao, enviaremos um novo link.',
        ], 202, $request->requestId);
    }

    private function verifyEmail(Request $request): never
    {
        $this->auth->verifyEmail($request->string('token', 256));
        Response::json(['ok' => true], 200, $request->requestId);
    }

    private function mercadoPagoWebhook(Request $request): never
    {
        $dataId = (string) ($_GET['data_id'] ?? $_GET['id'] ?? '');
        $this->payments->receiveMercadoPagoWebhook($request, $dataId);
        Response::empty(200, $request->requestId);
    }

    private function logout(Request $request): never
    {
        $this->sessions->logout($request);
        Response::empty(204, $request->requestId);
    }

    private function deactivateDevice(Request $request): never
    {
        $user = $this->sessions->requireUser($request, true);
        $this->licenses->deactivateCurrentDevice((string) $user['id'], $request);
        Response::json(['ok' => true], 200, $request->requestId);
    }

    private function assertHttps(): void
    {
        if (!$this->config->isProduction()) {
            return;
        }
        $https = strtolower((string) ($_SERVER['HTTPS'] ?? '')) === 'on';
        $remote = (string) ($_SERVER['REMOTE_ADDR'] ?? '');
        if (!$https
            && $this->config->string('app.trusted_proxy') !== ''
            && TrustedProxy::contains(
                $this->config->string('app.trusted_proxy'),
                $remote
            )) {
            $https = strtolower((string) ($_SERVER['HTTP_X_FORWARDED_PROTO'] ?? '')) === 'https';
        }
        if (!$https) {
            throw new ApiException(400, 'HTTPS_REQUIRED', 'Conexão segura obrigatória.');
        }
        header('Strict-Transport-Security: max-age=31536000; includeSubDomains');
    }
}
