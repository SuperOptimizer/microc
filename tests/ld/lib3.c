/* Never referenced: must not be pulled out of the archive. */
int not_defined_anywhere(void);
int lib_unused(void)
{
    return not_defined_anywhere();
}
