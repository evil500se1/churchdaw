package com.churchdaw.qa;

import android.os.Bundle;
import android.graphics.Rect;
import android.view.KeyEvent;

import com.android.uiautomator.core.UiDevice;
import com.android.uiautomator.core.UiObject;
import com.android.uiautomator.core.UiSelector;
import com.android.uiautomator.testrunner.UiAutomatorTestCase;

/**
 * Driver externo de QA. Ele nao e empacotado no ChurchDAW e roda somente pelo
 * comando "uiautomator runtest" no aparelho de teste.
 */
public final class ChurchDawLoginDriver extends UiAutomatorTestCase {
    private static final long FIELD_TIMEOUT_MS = 20_000L;
    private static final long LOGIN_TIMEOUT_MS = 30_000L;

    @Override
    protected void setUp() throws Exception {
        super.setUp();
        getUiDevice().wakeUp();
    }

    public void testLogin() throws Exception {
        Bundle params = getParams();
        String email = params.getString("email");
        String code = params.getString("code");
        assertNotNull("Parametro email ausente", email);
        assertNotNull("Parametro code ausente", code);

        UiObject emailField = new UiObject(
            new UiSelector().descriptionContains("E-mail,E-mail"));
        UiObject passwordField = new UiObject(
            new UiSelector().descriptionContains("Senha,Senha"));
        assertTrue(
            "Campo E-mail nao apareceu",
            emailField.waitForExists(FIELD_TIMEOUT_MS));
        assertTrue(
            "Campo Senha nao apareceu",
            passwordField.waitForExists(FIELD_TIMEOUT_MS));

        UiDevice device = getUiDevice();
        replaceText(device, emailField, email);
        device.pressEnter();
        Thread.sleep(800L);
        replaceFocusedText(device, passwordField, code);
        device.pressBack();

        UiObject enter = new UiObject(
            new UiSelector().description("Entrar"));
        assertTrue(
            "Botao Entrar nao apareceu",
            enter.waitForExists(FIELD_TIMEOUT_MS));
        assertTrue("Botao Entrar nao habilitou", enter.isEnabled());
        enter.click();

        UiObject mainUi = new UiObject(
            new UiSelector().descriptionContains("Timbres"));
        assertTrue(
            "A DAW nao abriu apos o login",
            mainUi.waitForExists(LOGIN_TIMEOUT_MS));
    }

    private static void replaceText(
        UiDevice device, UiObject field, String value) throws Exception {
        Rect bounds = field.getBounds();
        device.click(bounds.centerX(), bounds.centerY());
        Thread.sleep(350L);
        replaceFocusedText(device, field, value);
    }

    private static void replaceFocusedText(
        UiDevice device, UiObject field, String value) throws Exception {
        device.pressKeyCode(
            KeyEvent.KEYCODE_A, KeyEvent.META_CTRL_ON);
        device.pressDelete();
        Thread.sleep(250L);
        assertTrue(
            "Nao foi possivel preencher campo",
            field.setText(value));
    }
}
