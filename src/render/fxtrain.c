static int fxr_main(int argc, char **argv) {
    if (argc >= 1 && !strcmp(argv[0], "selftest-grad")) return fxr_selftest_grad();
    fprintf(stderr, "uso: katana render flux-train selftest-grad\n");
    return 1;
}
