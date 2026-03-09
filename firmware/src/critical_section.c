#include <zephyr/kernel.h>

static unsigned int cs_irq_key;

void _critical_section_1_0_acquire(void)
{
    cs_irq_key = irq_lock();
}

void _critical_section_1_0_release(void)
{
    irq_unlock(cs_irq_key);
}