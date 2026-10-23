/* Keep the core running until the remote-bitbang debugger ends the test. */
int main(void) {
    for (;;) __asm__ volatile ("nop");
}
